// Probe rays for single grid points.
//
// A probe casts down from the probe height above the terrain to just below the terrain.
// The grid point is occluded if the ray hits an occluder before the land.
// A ray that starts inside an occluder's collision misses it (Experimental Acoustics finding).
// So a probe that finds nothing on the way down casts up again from just above the terrain.

#include "probe/probe.h"

#include "engine/addresses.h"
#include "engine/objects.h"
#include "engine/terrain.h"
#include "utils/vector3.h"

namespace sof::probe {

	namespace {

		// Engine AI sight ray. The ray layer does not filter hits, so the probe filters by hit layer and reference.
		constexpr std::uint8_t kRayLayer = engine::kLayer_LineOfSight;

		// Distance to skip past a hit before the next cast.
		constexpr float kStepPastHit = 1.0f;  // Game units

		// End of the down ray below the terrain. Covers the gap between land height and terrain collision.
		constexpr float kDepthBelowTerrain = 8.0f;  // Game units

		// Start of the up ray above the terrain.
		constexpr float kHeightAboveTerrain = 1.0f;  // Game units

		// Hits per ray before the probe gives up on that ray. Clutter and actors on top of a floor add hits.
		constexpr int kMaxHitsPerRay = 8;

		// Returns the role of a hit. The reference decides before the layer.
		// Land collision has no reference and uses layer `Ground`.
		HitKind ClassifyHit(const engine::RaycastResult& hit) {
			if (IsOccluder(hit.reference)) {
				return HitKind::Occluder;
			}

			if (!hit.reference && (hit.layer == engine::kLayer_Ground || hit.layer == engine::kLayer_Terrain)) {
				return HitKind::Terrain;
			}

			return HitKind::Passed;
		}

		// Casts between two points and again from just past each hit that is not an occluder.
		// Returns the first occluder hit, or null.
		// A down ray ends at the terrain. An up ray passes terrain, which is an overhang there.
		void* CastThrough(const Vector3& from, const Vector3& to, bool upward, ProbeResult& result,
			const TraceCallback& trace) {
			const Vector3 step = Normalize(to - from) * kStepPastHit;
			const float totalLength = Distance(from, to);
			Vector3 start = from;

			for (int hitIndex = 0; hitIndex < kMaxHitsPerRay; ++hitIndex) {
				const engine::RaycastResult hit = engine::CastRay(start, to, kRayLayer);
				++result.casts;

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

	const char* VerdictName(Verdict verdict) {
		switch (verdict) {
		case Verdict::Clear:
			return "Clear";

		case Verdict::Occluded:
			return "Occluded";

		case Verdict::NoTerrain:
			return "NoTerrain";

		default:
			return "?";
		}
	}

	// Checks if a given reference hides grass under it. Statics and static collections do.
	bool IsOccluder(void* reference) {
		if (!reference) {
			return false;
		}

		const std::uint8_t baseType = engine::GetFormType(engine::GetBaseForm(reference));

		return baseType == engine::kFormType_TESObjectSTAT || baseType == engine::kFormType_BGSStaticCollection;
	}

	// Probes the grid point at the given world position for an occluder within the given height above the terrain.
	// Calls the trace callback for each hit, if one is given.
	//
	// Thread: Main
	ProbeResult ProbePoint(float x, float y, float probeHeight, const TraceCallback& trace) {
		ProbeResult result;

		if (!engine::GetLandHeight({x, y, 0.0f}, result.terrainHeight)) {
			result.verdict = Verdict::NoTerrain;

			return result;
		}

		const Vector3 top{x, y, result.terrainHeight + probeHeight};
		const Vector3 bottom{x, y, result.terrainHeight - kDepthBelowTerrain};

		result.occluder = CastThrough(top, bottom, false, result, trace);

		if (!result.occluder) {
			const Vector3 ground{x, y, result.terrainHeight + kHeightAboveTerrain};

			result.occluder = CastThrough(ground, top, true, result, trace);
			result.foundByUpRay = result.occluder != nullptr;
		}

		if (result.occluder) {
			result.verdict = Verdict::Occluded;
		}

		return result;
	}

}
