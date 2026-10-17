#include "engine/terrain.h"

#include "engine/addresses.h"
#include "utils/memory.h"

namespace sof::engine {

	// Reads the terrain height under a given position into the given height, in game units.
	// Returns false if no loaded cell holds the position. Only x and y of the position count.
	//
	// Thread: Main
	bool GetLandHeight(const Vector3& position, float& height) {
		void* tes = mem::Global<void*>(kTESSingleton);

		if (!tes) {
			return false;
		}

		using GetLandHeightFn = bool(__thiscall*)(void* tes, const Vector3* position, float* height);

		const auto getLandHeight = reinterpret_cast<GetLandHeightFn>(kTES_GetLandHeight);

		return getLandHeight(tes, &position, &height);
	}

}
