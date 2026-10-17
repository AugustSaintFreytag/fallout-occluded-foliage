#pragma once

#include "utils/vector3.h"

#include <cstdint>
#include <vector>

namespace sof::engine {

	// An exterior cell in the loaded grid.
	struct LoadedCell {
		void* cell = nullptr;  // TESObjectCELL*
		std::int32_t x = 0;
		std::int32_t y = 0;
	};

	// Horizontal world bounds of a reference's loaded 3D.
	struct ReferenceBounds {
		Vector3 center;  // Game units
		float radius = 0.0f;  // Game units
	};

	std::vector<LoadedCell> GetLoadedExteriorCells();

	void* GetWorldSpace(void* cell);
	bool IsInterior(void* cell);
	bool IsAttached(void* cell);

	std::vector<void*> GetCellReferences(void* cell);

	bool IsDisabledOrDeleted(void* reference);
	void* GetRootNode(void* reference);
	bool GetWorldBounds(void* reference, ReferenceBounds& bounds);

}
