// Discovery tools for grass geometry and its instance records (milestone 3).
//
// The census logs every grass geometry under the grass root node, nearest first.
// It checks the instance layout against the land height and compares records with the previous census.
// The hide test moves every instance of the nearest grass geometry far below the land, and back on the next press.

#include "debug/grass_census.h"

#include "engine/addresses.h"
#include "engine/grass.h"
#include "engine/objects.h"
#include "engine/terrain.h"
#include "engine/ui.h"
#include "utils/log.h"
#include "utils/memory.h"
#include "utils/vector3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace sof::debug {

	namespace {

		constexpr std::size_t kMaxLoggedGeometries = 32;
		constexpr std::size_t kMaxDumpedRecords = 4;
		constexpr std::size_t kLoggedInstances = 3;
		constexpr std::size_t kRecordDumpWords = 20;

		// Instances closer than this to the land height count as on the land.
		constexpr float kOnLandTolerance = 64.0f;  // Game units

		// Depth by which the hide test moves instances. Far enough to leave the view frustum.
		constexpr float kHideDepth = 100000.0f;  // Game units

		// Instances below this height were moved by the hide test.
		constexpr float kHiddenBelow = -50000.0f;  // Game units

		constexpr float kCellSize = 4096.0f;  // Game units

		// State of a grass geometry at the last census.
		struct Snapshot {
			void* geometry = nullptr;
			void* record = nullptr;
			engine::GrassInstance* instances = nullptr;
			std::uint16_t count = 0;
			engine::GrassInstance first;
		};

		// A grass geometry whose instances the hide test moved.
		struct HiddenGeometry {
			void* geometry = nullptr;
			void* record = nullptr;
			engine::GrassInstance* instances = nullptr;
		};

		// Thread: Main
		std::vector<Snapshot> g_lastCensus;
		HiddenGeometry g_hidden;

		// Copies the instances of a given record into the given list. Returns false if the array cannot be read.
		bool ReadInstances(void* record, std::vector<engine::GrassInstance>& instances) {
			const std::uint16_t count = engine::GetInstanceCount(record);
			engine::GrassInstance* source = engine::GetInstances(record);

			instances.resize(count);

			if (count == 0) {
				return true;
			}

			return source && mem::SafeReadBytes(source, instances.data(), count * sizeof(engine::GrassInstance));
		}

		float HorizontalDistance(const Vector3& from, const Vector3& to) {
			return std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
		}

		// Returns the horizontal distance from a given position to the nearest instance of a given geometry.
		float NearestInstanceDistance(const engine::GrassGeometry& grass, const std::vector<engine::GrassInstance>& instances,
			const Vector3& position) {
			float nearest = 1.0e30f;

			for (const engine::GrassInstance& instance : instances) {
				const Vector3 world = engine::ObjectToWorld(grass.geometry, {instance.x, instance.y, instance.z});
				nearest = std::min(nearest, HorizontalDistance(position, world));
			}

			return nearest;
		}

		bool IsIdentityRotation(void* niObject) {
			const float* rotation = &mem::Field<float>(niObject, engine::kNiObject_WorldRotate);
			const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

			for (int index = 0; index < 9; ++index) {
				if (std::fabs(rotation[index] - identity[index]) > 0.001f) {
					return false;
				}
			}

			return true;
		}

		void LogRecordDump(void* record) {
			std::uint32_t words[kRecordDumpWords] = {};

			if (!mem::SafeReadBytes(record, words, sizeof(words))) {
				SOF_LOG("[Grass]     Record dump: unreadable");

				return;
			}

			char text[512];
			std::size_t length = 0;

			for (std::size_t index = 0; index < kRecordDumpWords && length < sizeof(text); ++index) {
				length += std::snprintf(text + length, sizeof(text) - length, " +%02zX=%08X", index * 4, words[index]);
			}

			SOF_LOG("[Grass]     Record dump:%s", text);
		}

		// Logs one geometry: its record, transform, the first instances in world space and how many lie on the land.
		void LogGeometry(std::size_t index, const engine::GrassGeometry& grass, const std::vector<engine::GrassInstance>& instances,
			float distance, std::size_t shareCount) {
			const Vector3 origin = engine::ObjectToWorld(grass.geometry, {});
			const float scale = mem::Field<float>(grass.geometry, engine::kNiObject_WorldScale);
			const std::uint32_t flags2 = mem::Field<std::uint32_t>(grass.property, engine::kShaderProperty_Flags2);

			// Candidates for an instance count on the geometry. xNVSE places it at +0xC0 with an older `NiGeometry` size.
			const std::uint16_t geometryCountC0 = mem::Field<std::uint16_t>(grass.geometry, 0xC0);
			const std::uint16_t geometryCountC4 = mem::Field<std::uint16_t>(grass.geometry, 0xC4);

			SOF_LOG("[Grass]   #%zu %s Geometry=%p Property=%p Flags2=%08X Record=%p (shared by %zu) Capacity=%u Count=%u "
					"Array=%p Geometry+C0=%u +C4=%u Origin=(%.0f, %.0f, %.0f) Scale=%.2f Rotation=%s Nearest=%.0f",
				index, grass.isStrips ? "Strips" : "Shape", grass.geometry, grass.property, flags2, grass.record, shareCount,
				engine::GetCapacity(grass.record), engine::GetInstanceCount(grass.record), engine::GetInstances(grass.record),
				geometryCountC0, geometryCountC4, origin.x,
				origin.y, origin.z, scale, IsIdentityRotation(grass.geometry) ? "identity" : "rotated", distance);

			std::size_t onLand = 0;
			std::size_t withLand = 0;
			float maxOffset = 0.0f;

			for (std::size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex) {
				const engine::GrassInstance& instance = instances[instanceIndex];
				const Vector3 world = engine::ObjectToWorld(grass.geometry, {instance.x, instance.y, instance.z});
				float land = 0.0f;
				const bool hasLand = engine::GetLandHeight(world, land);

				if (hasLand) {
					++withLand;
					maxOffset = std::max(maxOffset, std::fabs(world.z - land));

					if (std::fabs(world.z - land) <= kOnLandTolerance) {
						++onLand;
					}
				}

				if (instanceIndex < kLoggedInstances) {
					SOF_LOG("[Grass]     Instance %zu: local (%.1f, %.1f, %.1f, w %.3f) -> world (%.1f, %.1f, %.1f), land %s%.1f",
						instanceIndex, instance.x, instance.y, instance.z, instance.w, world.x, world.y, world.z,
						hasLand ? "" : "none ", land);
				}
			}

			SOF_LOG("[Grass]     %zu of %zu instances with land lie within %.0f units of it. Largest offset %.1f.", onLand,
				withLand, kOnLandTolerance, maxOffset);
		}

		// Logs how the given geometries changed since the last census, then remembers them.
		void CompareWithLastCensus(const std::vector<engine::GrassGeometry>& geometries) {
			std::size_t unchanged = 0;
			std::size_t changed = 0;
			std::size_t added = 0;
			std::vector<Snapshot> census;

			for (const engine::GrassGeometry& grass : geometries) {
				Snapshot snapshot{grass.geometry, grass.record, engine::GetInstances(grass.record),
					engine::GetInstanceCount(grass.record), {}};

				if (snapshot.instances && snapshot.count > 0) {
					mem::SafeReadBytes(snapshot.instances, &snapshot.first, sizeof(snapshot.first));
				}

				const auto previous = std::find_if(g_lastCensus.begin(), g_lastCensus.end(),
					[&](const Snapshot& entry) { return entry.geometry == grass.geometry; });

				if (previous == g_lastCensus.end()) {
					++added;
				} else if (previous->record == snapshot.record && previous->instances == snapshot.instances
					&& previous->count == snapshot.count && previous->first.x == snapshot.first.x
					&& previous->first.y == snapshot.first.y && previous->first.z == snapshot.first.z) {
					++unchanged;
				} else {
					++changed;
				}

				census.push_back(snapshot);
			}

			if (!g_lastCensus.empty()) {
				SOF_LOG("[Grass] Since the last census: %zu unchanged, %zu changed record or array, %zu new geometries, "
						"%zu of the previous geometries gone.",
					unchanged, changed, added, g_lastCensus.size() - unchanged - changed);
			}

			g_lastCensus = std::move(census);
		}

		// Returns the grass geometry with the instance nearest to the player, or null.
		const engine::GrassGeometry* FindNearestGeometry(const std::vector<engine::GrassGeometry>& geometries,
			const Vector3& position, float& distance) {
			const engine::GrassGeometry* nearest = nullptr;
			distance = 1.0e30f;
			std::vector<engine::GrassInstance> instances;

			for (const engine::GrassGeometry& grass : geometries) {
				if (!ReadInstances(grass.record, instances) || instances.empty()) {
					continue;
				}

				const float geometryDistance = NearestInstanceDistance(grass, instances, position);

				if (geometryDistance < distance) {
					distance = geometryDistance;
					nearest = &grass;
				}
			}

			return nearest;
		}

		void RestoreHiddenGeometry() {
			const std::vector<engine::GrassGeometry> geometries = engine::CollectGrassGeometries();

			const bool stillThere = std::any_of(geometries.begin(), geometries.end(), [](const engine::GrassGeometry& grass) {
				return grass.geometry == g_hidden.geometry && grass.record == g_hidden.record
					&& engine::GetInstances(grass.record) == g_hidden.instances;
			});

			if (stillThere) {
				// Raise only marked instances. The game may have removed or added instances since the hide.
				const std::uint16_t count = engine::GetInstanceCount(g_hidden.record);
				std::size_t restored = 0;

				for (std::uint16_t index = 0; index < count; ++index) {
					if (g_hidden.instances[index].z < kHiddenBelow) {
						g_hidden.instances[index].z += kHideDepth;
						++restored;
					}
				}

				SOF_LOG("[Grass] Hide test: restored %zu of %u instances of geometry %p.", restored, count, g_hidden.geometry);
				engine::ShowNotification("Grass hide test: restored");
			} else {
				SOF_LOG("[Grass] Hide test: geometry %p or its record is gone. The game rebuilt it, nothing restored.",
					g_hidden.geometry);
				engine::ShowNotification("Grass hide test: geometry gone");
			}

			g_hidden = {};
		}

	}

	// Logs every grass geometry under the grass root node, nearest to the player first.
	// Logs record fields, transforms, the first instances in world space and how many instances lie on the land.
	// Compares records and arrays with the previous census.
	//
	// Thread: Main
	void RunGrassCensus() {
		void* root = engine::GetGrassRoot();

		if (!root) {
			SOF_LOG("[Grass] No grass root node.");
			engine::ShowNotification("Grass census: no grass root");

			return;
		}

		const std::vector<engine::GrassGeometry> geometries = engine::CollectGrassGeometries();
		const Vector3 player = engine::GetPosition(engine::GetPlayer());

		std::unordered_map<void*, std::size_t> recordUsers;
		std::size_t totalInstances = 0;
		std::size_t withoutRecord = 0;

		for (const engine::GrassGeometry& grass : geometries) {
			if (!grass.record) {
				++withoutRecord;
				continue;
			}

			++recordUsers[grass.record];
			totalInstances += engine::GetInstanceCount(grass.record);
		}

		SOF_LOG("[Grass] Census: root %p, %zu geometries, %zu without a record, %zu distinct records, %zu instances. "
				"Player at (%.0f, %.0f, %.0f), cell (%d, %d).",
			root, geometries.size(), withoutRecord, recordUsers.size(), totalInstances, player.x, player.y, player.z,
			static_cast<int>(std::floor(player.x / kCellSize)), static_cast<int>(std::floor(player.y / kCellSize)));

		// Order by distance to the nearest instance.
		struct Entry {
			const engine::GrassGeometry* grass;
			std::vector<engine::GrassInstance> instances;
			float distance;
		};

		std::vector<Entry> entries;

		for (const engine::GrassGeometry& grass : geometries) {
			Entry entry{&grass, {}, 1.0e30f};

			if (grass.record && ReadInstances(grass.record, entry.instances) && !entry.instances.empty()) {
				entry.distance = NearestInstanceDistance(grass, entry.instances, player);
			}

			entries.push_back(std::move(entry));
		}

		std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
			return left.distance < right.distance;
		});

		for (std::size_t index = 0; index < entries.size() && index < kMaxLoggedGeometries; ++index) {
			const Entry& entry = entries[index];
			const std::size_t shareCount = entry.grass->record ? recordUsers[entry.grass->record] : 0;

			LogGeometry(index, *entry.grass, entry.instances, entry.distance, shareCount);

			if (index < kMaxDumpedRecords && entry.grass->record) {
				LogRecordDump(entry.grass->record);
			}
		}

		CompareWithLastCensus(geometries);

		char notification[96];
		std::snprintf(notification, sizeof(notification), "Grass census: %zu geometries, %zu instances", geometries.size(),
			totalInstances);
		engine::ShowNotification(notification);
	}

	// Moves every instance of the grass geometry nearest to the player far below the land.
	// On the next press, moves them back if the geometry and its array still exist.
	//
	// Thread: Main
	void ToggleGrassHideTest() {
		if (g_hidden.geometry) {
			RestoreHiddenGeometry();

			return;
		}

		const std::vector<engine::GrassGeometry> geometries = engine::CollectGrassGeometries();
		const Vector3 player = engine::GetPosition(engine::GetPlayer());

		float distance = 0.0f;
		const engine::GrassGeometry* nearest = FindNearestGeometry(geometries, player, distance);

		if (!nearest) {
			SOF_LOG("[Grass] Hide test: no grass geometry with instances.");
			engine::ShowNotification("Grass hide test: no grass");

			return;
		}

		engine::GrassInstance* instances = engine::GetInstances(nearest->record);
		const std::uint16_t count = engine::GetInstanceCount(nearest->record);

		const std::size_t shareCount = std::count_if(geometries.begin(), geometries.end(),
			[&](const engine::GrassGeometry& grass) { return grass.record == nearest->record; });

		g_hidden.geometry = nearest->geometry;
		g_hidden.record = nearest->record;
		g_hidden.instances = instances;

		for (std::uint16_t index = 0; index < count; ++index) {
			instances[index].z -= kHideDepth;
		}

		SOF_LOG("[Grass] Hide test: moved %u instances (capacity %u) of geometry %p down by %.0f. Nearest instance %.0f units "
				"away. Record %p is shared by %zu geometries.",
			count, engine::GetCapacity(nearest->record), nearest->geometry, kHideDepth, distance, nearest->record, shareCount);
		engine::ShowNotification("Grass hide test: nearest geometry hidden");
	}

}
