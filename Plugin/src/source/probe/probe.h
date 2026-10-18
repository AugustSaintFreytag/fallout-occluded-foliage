#pragma once

#include "engine/havok.h"

#include <cstdint>
#include <functional>
#include <unordered_set>

namespace sof::probe {

	// Outcome of a probe at one grid point.
	enum class Verdict : std::uint8_t {
		Clear,
		Occluded,
		NoTerrain,  // No loaded land under the grid point
	};

	// Role of a hit in a probe ray.
	enum class HitKind : std::uint8_t {
		Passed,  // Not an occluder, the ray continues past it
		Occluder,
		Terrain,
	};

	// A hit of a probe ray, for a debug trace.
	struct TraceHit {
		engine::RaycastResult hit;
		HitKind kind = HitKind::Passed;
		bool upward = false;
		float heightAboveTerrain = 0.0f;  // Game units
	};

	using TraceCallback = std::function<void(const TraceHit& traceHit)>;

	// Result of a probe at one grid point.
	struct ProbeResult {
		Verdict verdict = Verdict::Clear;
		float terrainHeight = 0.0f;  // Game units
		void* occluder = nullptr;  // TESObjectREFR* of the occluder, null unless occluded
		bool foundByUpRay = false;
		bool faulted = false;  // An engine pick raised an exception. The verdict is not valid.
		int casts = 0;
	};

	const char* DescriptionForVerdict(Verdict verdict);

	bool IsOccluder(void* reference);

	void SetOccluderBlacklist(std::unordered_set<std::uint32_t> baseFormIDs);

	ProbeResult ProbePoint(void* cell, float x, float y, float terrainHeight, float probeHeight, const TraceCallback& trace = {});

}
