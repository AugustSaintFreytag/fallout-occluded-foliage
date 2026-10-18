// Hides grass instances that grow on occluded grid points.
//
// New instances pass through a hook on the only `AddInstance` call (0xB62FC1).
// An instance on an occluded grid point goes in with its z lowered far below the land.
// Instances added before their grid point resolved are lowered in place by a pass over the instance records.
// The pass visits each record whose cell map gained occluded grid points since the record's last pass.
// It runs under a time budget per frame and goes on in the next frame.
//
// With `[Debug] bLiftGrass`, occluded instances are raised by a small height instead, to show the map in game.
// A change of mode or `[General] bEnabled` restores every marked instance first.

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

		constexpr float kLiftHeight = 64.0f;  // Game units
		// Lifted instances lie at least this far above the land. Unmarked grass lies 0.5–1.3 units above it.
		constexpr float kLiftedAbove = 32.0f;  // Game units

		constexpr double kPassBudgetMicroseconds = 150.0;
		constexpr double kStatusIntervalSeconds = 5.0;

		enum class Mode : std::uint8_t {
			Off,
			Hide,
			Lift,
		};

		using AddInstanceFn = bool(__thiscall*)(void* record, const engine::GrassInstance* instance, void* key);

		// The cell map state a record was last passed with.
		struct AppliedRecord {
			std::uint32_t serial = 0;
			std::uint32_t version = 0;
		};

		// Counters for the status line.
		struct Statistics {
			std::uint32_t markedOnAdd = 0;
			std::uint32_t markedInPass = 0;
			std::uint32_t passedRecords = 0;
			std::uint32_t passFrames = 0;
			double passMicroseconds = 0.0;
			double maxPassMicroseconds = 0.0;
		};

		// Thread: Main
		bool g_installed = false;
		Mode g_mode = Mode::Off;
		std::unordered_map<void*, AppliedRecord> g_applied;
		bool g_passPending = false;
		std::uint32_t g_seenVersion = 0;  // Occlusion version at the last check
		Statistics g_statistics;
		std::int64_t g_lastStatusTime = 0;

		// Thread: Any
		std::atomic<std::uint32_t> g_offThreadCalls{0};

		Mode ModeFromSettings(const config::Settings& settings) {
			if (!settings.general.enabled) {
				return Mode::Off;
			}

			return settings.debug.liftGrass ? Mode::Lift : Mode::Hide;
		}

		// Returns the land height under a given world position from the land samples of its cell map.
		// Returns false if the cell has no map or no land samples.
		bool LandHeightUnder(float x, float y, float& height) {
			const GridPoint point = ToGridPoint(x, y);
			const CellMap* map = FindCellMap(CellOf(point.x), CellOf(point.y));

			return map && LandHeightAt(*map, x, y, height);
		}

		// Checks if a given instance carries the mark of a given mode.
		// A lifted instance is recognized by its height above the land. Without land samples it counts as unmarked.
		bool IsMarked(const engine::GrassInstance& instance, Mode mode) {
			if (mode == Mode::Hide) {
				return instance.z < kHiddenBelow;
			}

			float land = 0.0f;

			return mode == Mode::Lift && LandHeightUnder(instance.x, instance.y, land) && instance.z - land > kLiftedAbove;
		}

		void Mark(engine::GrassInstance& instance, Mode mode) {
			instance.z += mode == Mode::Hide ? -kHideDepth : kLiftHeight;
		}

		void Unmark(engine::GrassInstance& instance, Mode mode) {
			instance.z -= mode == Mode::Hide ? -kHideDepth : kLiftHeight;
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
		// An instance on an occluded grid point goes in marked. Others go in unchanged.
		// Off the main thread, every instance goes in unchanged, as the maps belong to the main thread.
		//
		// Thread: Main (scatter loop, likely)
		bool __fastcall AddInstanceHook(void* record, void*, const engine::GrassInstance* instance, void* key) {
			const auto addInstance = reinterpret_cast<AddInstanceFn>(engine::kGrassRecord_AddInstance);

			if (!threads::IsMainThread()) {
				g_offThreadCalls.fetch_add(1, std::memory_order_relaxed);

				return addInstance(record, instance, key);
			}

			if (g_mode == Mode::Off || !instance || Query(instance->x, instance->y) != PointState::Occluded) {
				return addInstance(record, instance, key);
			}

			engine::GrassInstance marked = *instance;
			Mark(marked, g_mode);
			++g_statistics.markedOnAdd;

			return addInstance(record, &marked, key);
		}

		// Marks the unmarked instances of a given record that lie on occluded grid points.
		// Returns the number of instances marked.
		std::uint32_t PassRecord(void* record) {
			std::uint16_t count = 0;
			engine::GrassInstance* instances = RecordInstances(record, count);

			if (!instances) {
				return 0;
			}

			std::uint32_t marked = 0;

			for (std::uint16_t index = 0; index < count; ++index) {
				engine::GrassInstance& instance = instances[index];

				if (IsMarked(instance, g_mode) || Query(instance.x, instance.y) != PointState::Occluded) {
					continue;
				}

				Mark(instance, g_mode);
				++marked;
			}

			return marked;
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

				g_statistics.markedInPass += PassRecord(grass.record);
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

		// Removes the mark of a given mode from every instance under the grass root. Logs the number restored.
		void RestoreAll(Mode mode) {
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
					if (IsMarked(instances[index], mode)) {
						Unmark(instances[index], mode);
						++restored;
					}
				}
			}

			SOF_LOG("[Grass] Restored %s %s instances.", format::Count(restored).c_str(), mode == Mode::Hide ? "hidden" : "lifted");
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

			if (g_statistics.markedOnAdd > 0 || g_statistics.passFrames > 0) {
				SOF_LOG("[Grass] %s %s instances on add and %s in passes over %s records. "
						"Passes in %s frames, %.3f ms per frame, %.3f ms max.",
					g_mode == Mode::Lift ? "Lifted" : "Hid", format::Count(g_statistics.markedOnAdd).c_str(),
					format::Count(g_statistics.markedInPass).c_str(), format::Count(g_statistics.passedRecords).c_str(),
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

	// Follows the mode from the settings and runs the pass over the instance records when maps changed.
	// Runs while `[General] bEnabled` is off too, so a change to off restores the grass.
	//
	// Thread: Main (each frame)
	void UpdateHider() {
		if (!g_installed) {
			return;
		}

		const config::Settings& settings = config::Get();
		const Mode mode = ModeFromSettings(settings);

		if (mode != g_mode) {
			if (g_mode != Mode::Off) {
				RestoreAll(g_mode);
			}

			g_mode = mode;
			g_applied.clear();
			g_passPending = mode != Mode::Off;
		}

		if (g_mode == Mode::Off) {
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

	// Forgets which records were passed. Called with the builder reset, on game load and exit to the main menu.
	// Marked instances stay marked. The next pass skips them by their z.
	//
	// Thread: Main
	void ResetHider() {
		g_applied.clear();
		g_passPending = g_mode != Mode::Off;
		g_statistics = {};
		g_lastStatusTime = 0;
	}

}
