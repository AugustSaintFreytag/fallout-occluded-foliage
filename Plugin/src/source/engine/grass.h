#pragma once

#include "utils/vector3.h"

#include <cstdint>
#include <vector>

namespace sof::engine {

	// One grass instance as the shader reads it from `c20[a0.x]`.
	struct GrassInstance {
		float x = 0.0f;  // World position, game units
		float y = 0.0f;
		float z = 0.0f;
		float w = 0.0f;  // Meaning not verified
	};

	static_assert(sizeof(GrassInstance) == 16);

	// A grass geometry under the grass root node, with its shader property and instance record.
	struct GrassGeometry {
		void* geometry = nullptr;  // TallGrassTriShape* or TallGrassTriStrips*
		void* property = nullptr;  // TallGrassShaderProperty*
		void* record = nullptr;  // Instance record, can be null
		bool isStrips = false;
	};

	void* GetGrassRoot();

	std::vector<GrassGeometry> CollectGrassGeometries();

	std::uint16_t GetCapacity(void* record);
	std::uint16_t GetInstanceCount(void* record);
	GrassInstance* GetInstances(void* record);

	Vector3 ObjectToWorld(void* niObject, const Vector3& local);

}
