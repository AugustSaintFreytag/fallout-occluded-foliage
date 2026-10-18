// Raycast Occluder Probing
//
// A probe casts down from the probe height above the terrain to just below the terrain.
// A grid point is occluded if the ray hits a valid occluder between start and terrain.
// Rays starting already inside an occluder will not register as hit. 
// Casting again from an offset position can find large occluders missed from default height.

#include "probe/probe.h"

#include "engine/addresses.h"
#include "engine/objects.h"
#include "utils/vector3.h"

namespace sof::probe {

	namespace {

		// Ray layer to use for probing.
		// Layer `kLayer_LineOfSight` is used by engine for line of sight detection.
		constexpr std::uint8_t kRayLayer = engine::kLayer_LineOfSight;

		// Stepping distance/stride for raycasts (game units).
		constexpr float kStepPastHit = 1.0f;

		// Tolerance between assumed land height and actual terrain collision (game units).
		constexpr float kDepthBelowTerrain = 16.0f;

		// Height to start casting from when casting upwards from terrain (reverse probe) (game units).
		constexpr float kHeightAboveTerrain = 1.0f;

		// Number of allowed hits per ray before probe assumes occlusion.
		constexpr int kMaxHitsPerRay = 8;

		// Classifies and returns the hit kind for the given individual raycast result.
		HitKind ClassifyHit(const engine::RaycastResult& hit) {
			if (IsOccluder(hit.reference)) {
				return HitKind::Occluder;
			}

			if (!hit.reference && (hit.layer == engine::kLayer_Ground || hit.layer == engine::kLayer_Terrain)) {
				return HitKind::Terrain;
			}

			return HitKind::Passed;
		}

		// Casts between two points and returns the hit reference or null.
		// When ray did not hit, apply offset and retry for max hits per ray.
		// Returns the first occluder hit, or null.
		void* CastThrough(void* cell, const Vector3& from, const Vector3& to, bool upward, ProbeResult& result,
			const TraceCallback& trace) {
			const Vector3 step = Normalize(to - from) * kStepPastHit;
			const float totalLength = Distance(from, to);
			Vector3 start = from;

			for (int hitIndex = 0; hitIndex < kMaxHitsPerRay; ++hitIndex) {
				const engine::RaycastResult hit = engine::CastRayInCell(cell, start, to, kRayLayer);
				++result.casts;

				if (hit.faulted) {
					result.faulted = true;
					return nullptr;
				}

				if (!hit.hit) {
					return nullptr;
				}

				const HitKind kind = ClassifyHit(hit);

				if (trace) {
					trace({hit, kind, upward, hit.point.z - result.terrainHeight});
				}

				if (kind == HitKind::Occluder) {
					return hit.reference;
				}

				if (kind == HitKind::Terrain && !upward) {
					return nullptr;
				}

				start = hit.point + step;

				if (Distance(from, start) >= totalLength) {
					return nullptr;
				}
			}

			return nullptr;
		}

	}

	const char* DescriptionForVerdict(Verdict verdict) {
		switch (verdict) {
		case Verdict::Clear:
			return "Clear";

		case Verdict::Occluded:
			return "Occluded";

		case Verdict::NoTerrain:
			return "NoTerrain";

		default:
			return "Unknown";
		}
	}

	// Checks if a given reference should be a foliage occluder.
	//
	// Engine built-in statics do not have associated meshes (e.g. markers).
	// Built-in `CollisionMarker` has no 3D mesh but still has collision (not relevant for us).
	bool IsOccluder(void* reference) {
		if (!reference) {
			return false;
		}

		void* baseForm = engine::GetBaseForm(reference);
		const std::uint8_t baseType = engine::GetFormType(baseForm);

		if (baseType != engine::kFormType_TESObjectSTAT && baseType != engine::kFormType_BGSStaticCollection) {
			return false;
		}

		return engine::GetFormID(baseForm) >= engine::kPluginHeadFormID;
	}

	// Probes the grid point at a given world position for an occluder within the probe height above the terrain.
	// Casts in the Havok world of the given cell. Calls the trace callback for each hit, if one is given.
	// When faulting, the created result does not hold a verdict.
	//
	// Thread: Any (off-main, caller must lock/release)
	ProbeResult ProbePoint(void* cell, float x, float y, float terrainHeight, float probeHeight, const TraceCallback& trace) {
		ProbeResult result;
		result.terrainHeight = terrainHeight;

		const Vector3 top{x, y, terrainHeight + probeHeight};
		const Vector3 bottom{x, y, terrainHeight - kDepthBelowTerrain};

		result.occluder = CastThrough(cell, top, bottom, false, result, trace);

		if (!result.occluder && !result.faulted) {
			const Vector3 ground{x, y, terrainHeight + kHeightAboveTerrain};

			result.occluder = CastThrough(cell, ground, top, true, result, trace);
			result.foundByUpRay = result.occluder != nullptr;
		}

		if (result.occluder && !result.faulted) {
			result.verdict = Verdict::Occluded;
		}

		return result;
	}

}
