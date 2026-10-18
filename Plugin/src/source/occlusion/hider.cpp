// Hides grass instances that grow on occluded grid points.
//
// New instances pass through a hook on the only `AddInstance` call (0xB62FC1).
// An instance on an occluded grid point goes in with its z lowered far below the land.
// Instances added before their grid point resolved are lowered in place by a pass over the instance records.
// The pass visits each record whose cell map gained occluded grid points since the record's last pass.
// It runs under a time budget per frame and goes on in the next frame.
//
// Turning `[General] bEnabled` off restores every hidden instance. Turning it on hides them again from the maps.

#include "occlusion/hider.h"

#include "config/settings.h"
#include "engine/addresses.h"
#include "engine/grass.h"
#include "occlusion/map.h"
#include "utils/formatting.h"
#include "utils/log.h"
#include "utils/memory.h"
#include "utils/threads.h"
#include "utils/timing.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sof::occlusion {

	using mem::Field;

	namespace {

		// Same depth as the grass hide test, which worked in game
		constexpr float kHideDepth = 100000.0f;  // Game units
		constexpr float kHiddenBelow = -50000.0f;  // Hidden instances lie below this z

		constexpr double kPassBudgetMicroseconds = 150.0;
		constexpr double kStatusIntervalSeconds = 5.0;

		using AddInstanceFn = bool(__thiscall*)(void* record, const engine::GrassInstance* instance, void* key);

		// The cell map state a record was last passed with.
		struct AppliedRecord {
			std::uint32_t serial = 0;
			std::uint32_t version = 0;
		};

		// Counters for the status line.
		struct Statistics {
			std::uint32_t hiddenOnAdd = 0;
			std::uint32_t hiddenInPass = 0;
			std::uint32_t passedRecords = 0;
			std::uint32_t passFrames = 0;
			double passMicroseconds = 0.0;
			double maxPassMicroseconds = 0.0;
		};

		// Thread: Main
		bool g_installed = false;
		bool g_active = false;
		std::unordered_map<void*, AppliedRecord> g_applied;
		bool g_passPending = false;
		std::uint32_t g_seenVersion = 0;  // Occlusion version at the last check
		Statistics g_statistics;
		std::int64_t g_lastStatusTime = 0;

		// Thread: Any
		std::atomic<std::uint32_t> g_offThreadCalls{0};

		bool IsHidden(const engine::GrassInstance& instance) {
			return instance.z < kHiddenBelow;
		}

		// Returns the instance array of a given record with its count, or null if the count is past the capacity.
		engine::GrassInstance* RecordInstances(void* record, std::uint16_t& count) {
			count = engine::GetInstanceCount(record);

			if (count > engine::GetCapacity(record)) {
				return nullptr;
			}

			return engine::GetInstances(record);
		}

		// Replaces the original call to `AddInstance` in the grass scatter loop.
		// An instance on an occluded grid point goes in hidden. Others go in unchanged.
		// Off the main thread, every instance goes in unchanged, as the maps belong to the main thread.
		//
		// Thread: Main (scatter loop, likely)
		bool __fastcall AddInstanceHook(void* record, void*, const engine::GrassInstance* instance, void* key) {
			const auto addInstance = reinterpret_cast<AddInstanceFn>(engine::kGrassRecord_AddInstance);

			if (!threads::IsMainThread()) {
				g_offThreadCalls.fetch_add(1, std::memory_order_relaxed);

				return addInstance(record, instance, key);
			}

			if (!g_active || !instance || Query(instance->x, instance->y) != PointState::Occluded) {
				return addInstance(record, instance, key);
			}

			engine::GrassInstance hidden = *instance;
			hidden.z -= kHideDepth;
			++g_statistics.hiddenOnAdd;

			return addInstance(record, &hidden, key);
		}

		// Hides the visible instances of a given record that lie on occluded grid points.
		// Returns the number of instances hidden.
		std::uint32_t PassRecord(void* record) {
			std::uint16_t count = 0;
			engine::GrassInstance* instances = RecordInstances(record, count);

			if (!instances) {
				return 0;
			}

			std::uint32_t hidden = 0;

			for (std::uint16_t index = 0; index < count; ++index) {
				engine::GrassInstance& instance = instances[index];

				if (IsHidden(instance) || Query(instance.x, instance.y) != PointState::Occluded) {
					continue;
				}

				instance.z -= kHideDepth;
				++hidden;
			}

			return hidden;
		}

		// Passes every record whose cell map changed since its last pass, until the budget runs out.
		// Clears the pending flag once every record is up to date.
		//
		// Geometries that share a record are passed once. The list is collected again each frame,
		// so grass that loads or unloads between frames is handled.
		void RunPass() {
			const std::int64_t startTime = timing::Now();
			const std::vector<engine::GrassGeometry> geometries = engine::CollectGrassGeometries();
			bool finished = true;

			for (const engine::GrassGeometry& grass : geometries) {
				if (!grass.record) {
					continue;
				}

				const CellMap* map = FindCellMap(Field<std::int32_t>(grass.record, engine::kGrassRecord_CellX),
					Field<std::int32_t>(grass.record, engine::kGrassRecord_CellY));

				if (!map) {
					continue;
				}

				AppliedRecord& applied = g_applied[grass.record];

				if (applied.serial == map->serial && applied.version == map->occludedVersion) {
					continue;
				}

				if (timing::TicksToMicroseconds(static_cast<double>(timing::Now() - startTime)) >= kPassBudgetMicroseconds) {
					finished = false;
					break;
				}

				g_statistics.hiddenInPass += PassRecord(grass.record);
				++g_statistics.passedRecords;

				applied.serial = map->serial;
				applied.version = map->occludedVersion;
			}

			g_passPending = !finished;

			const double microseconds = timing::TicksToMicroseconds(static_cast<double>(timing::Now() - startTime));

			++g_statistics.passFrames;
			g_statistics.passMicroseconds += microseconds;
			g_statistics.maxPassMicroseconds = std::max(g_statistics.maxPassMicroseconds, microseconds);
		}

		// Brings back every hidden instance under the grass root. Logs the number restored.
		void RestoreAll() {
			std::unordered_set<void*> visited;
			std::uint32_t restored = 0;

			for (const engine::GrassGeometry& grass : engine::CollectGrassGeometries()) {
				if (!grass.record || !visited.insert(grass.record).second) {
					continue;
				}

				std::uint16_t count = 0;
				engine::GrassInstance* instances = RecordInstances(grass.record, count);

				if (!instances) {
					continue;
				}

				for (std::uint16_t index = 0; index < count; ++index) {
					if (IsHidden(instances[index])) {
						instances[index].z += kHideDepth;
						++restored;
					}
				}
			}

			SOF_LOG("[Grass] Restored %s hidden instances.", format::Count(restored).c_str());
		}

		void LogStatus(std::int64_t now) {
			if (g_lastStatusTime == 0) {
				g_lastStatusTime = now;

				return;
			}

			if (timing::TicksToMicroseconds(static_cast<double>(now - g_lastStatusTime)) < kStatusIntervalSeconds * 1.0e6) {
				return;
			}

			const std::uint32_t offThreadCalls = g_offThreadCalls.exchange(0, std::memory_order_relaxed);

			if (g_statistics.hiddenOnAdd > 0 || g_statistics.passFrames > 0) {
				SOF_LOG("[Grass] Hid %s instances on add and %s in passes over %s records. "
						"Passes in %s frames, %.3f ms per frame, %.3f ms max.",
					format::Count(g_statistics.hiddenOnAdd).c_str(), format::Count(g_statistics.hiddenInPass).c_str(), format::Count(g_statistics.passedRecords).c_str(),
					format::Count(g_statistics.passFrames).c_str(),
					g_statistics.passMicroseconds / std::max<std::uint32_t>(g_statistics.passFrames, 1) / 1000.0,
					g_statistics.maxPassMicroseconds / 1000.0);
			}

			if (offThreadCalls > 0) {
				SOF_LOG("[Grass] Warning: %s instances added off the main thread were left unchanged.",
					format::Count(offThreadCalls).c_str());
			}

			g_statistics = {};
			g_lastStatusTime = now;
		}

	}

	// Patches the `AddInstance` call in the grass scatter loop. Returns false if the call site does not match.
	//
	// Thread: Main (load time only)
	bool InstallHider() {
		g_installed = mem::PatchCall(engine::kGrassAddInstanceCall, engine::kGrassRecord_AddInstance,
			reinterpret_cast<void*>(&AddInstanceHook));

		if (g_installed) {
			SOF_LOG("[Grass] Patched the AddInstance call at %08X.", static_cast<std::uint32_t>(engine::kGrassAddInstanceCall));
		}

		return g_installed;
	}

	// Follows `[General] bEnabled` and runs the pass over the instance records when maps changed.
	// Runs while `bEnabled` is off too, so a change to off restores the grass.
	//
	// Thread: Main (each frame)
	void UpdateHider() {
		if (!g_installed) {
			return;
		}

		const config::Settings& settings = config::Get();

		if (settings.general.enabled != g_active) {
			if (g_active) {
				RestoreAll();
			}

			g_active = settings.general.enabled;
			g_applied.clear();
			g_passPending = g_active;
		}

		if (!g_active) {
			return;
		}

		// Any map that gained an occluded grid point needs a pass over its records
		const std::uint32_t version = GetOcclusionVersion();

		if (version != g_seenVersion) {
			g_seenVersion = version;
			g_passPending = true;
		}

		if (g_passPending) {
			RunPass();
		}

		if (settings.debug.logMap) {
			LogStatus(timing::Now());
		}
	}

	// Brings back every hidden instance and forgets which records were passed.
	// The next passes hide the grass again from the maps. Call when the maps no longer match the hidden grass.
	//
	// Thread: Main
	void RestoreHiddenGrass() {
		if (!g_installed || !g_active) {
			return;
		}

		RestoreAll();
		g_applied.clear();
		g_passPending = true;
	}

	// Forgets which records were passed. Called with the builder reset, on game load and exit to the main menu.
	// Hidden instances stay hidden. The next pass skips them by their z.
	//
	// Thread: Main
	void ResetHider() {
		g_applied.clear();
		g_passPending = g_active;
		g_statistics = {};
		g_lastStatusTime = 0;
	}

}
