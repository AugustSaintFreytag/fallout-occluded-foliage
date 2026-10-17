#include "engine/cells.h"

#include "engine/addresses.h"
#include "utils/memory.h"

namespace sof::engine {

	using mem::Field;

	namespace {

		// Upper limit for the grid size. The game default is 5, mods raise it to 7 or 9.
		constexpr std::uint32_t kMaxGridSize = 15;

		// Upper limit for the references of one cell. Guards against a broken list.
		constexpr std::size_t kMaxCellReferences = 65536;

	}

	// Returns the exterior cells of the loaded grid with their coordinates.
	// Empty in interiors and before the grid exists.
	//
	// Thread: Main
	std::vector<LoadedCell> GetLoadedExteriorCells() {
		std::vector<LoadedCell> cells;
		void* tes = mem::Global<void*>(kTESSingleton);

		if (!tes) {
			return cells;
		}

		void* grid = Field<void*>(tes, kTES_GridCellArray);

		if (!grid) {
			return cells;
		}

		const std::uint32_t gridSize = Field<std::uint32_t>(grid, kGridCellArray_GridSize);
		void** gridCells = Field<void**>(grid, kGridCellArray_Cells);

		if (!gridCells || gridSize == 0 || gridSize > kMaxGridSize) {
			return cells;
		}

		for (std::uint32_t index = 0; index < gridSize * gridSize; ++index) {
			void* cell = gridCells[index];

			if (!cell || IsInterior(cell)) {
				continue;
			}

			const std::int32_t* coords = Field<const std::int32_t*>(cell, kCell_ExteriorCoords);

			if (!coords) {
				continue;
			}

			cells.push_back({cell, coords[0], coords[1]});
		}

		return cells;
	}

	void* GetWorldSpace(void* cell) {
		if (!cell) {
			return nullptr;
		}

		return Field<void*>(cell, kCell_WorldSpace);
	}

	bool IsInterior(void* cell) {
		return cell && (Field<std::uint8_t>(cell, kCell_Flags) & kCellFlag_Interior) != 0;
	}

	bool IsAttached(void* cell) {
		return cell && Field<std::uint8_t>(cell, kCell_LoadingStage) == kCellStage_Attached;
	}

	// Returns the references in the object list of a given cell.
	//
	// Thread: Main
	std::vector<void*> GetCellReferences(void* cell) {
		std::vector<void*> references;

		if (!cell) {
			return references;
		}

		void* node = &Field<std::uint8_t>(cell, kCell_ObjectList);

		while (node && references.size() < kMaxCellReferences) {
			void* reference = Field<void*>(node, kListNode_Data);

			if (reference) {
				references.push_back(reference);
			}

			node = Field<void*>(node, kListNode_Next);
		}

		return references;
	}

	// Checks if a given reference is deleted or disabled, including references disabled by an enable parent.
	//
	// Reference at JIP "TESObjectREFR::GetDisabled".
	bool IsDisabledOrDeleted(void* reference) {
		using SetCheckFn = bool(__cdecl*)(void* reference);

		const std::uint32_t flags = Field<std::uint32_t>(reference, kForm_Flags);

		if (flags & kFormFlag_Deleted) {
			return true;
		}

		const auto inDisabledSet = reinterpret_cast<SetCheckFn>(kRefr_InDisabledSet);

		if (flags & kFormFlag_Disabled) {
			const auto inFlaggedEnabledSet = reinterpret_cast<SetCheckFn>(kRefr_InFlaggedEnabledSet);

			if (!inFlaggedEnabledSet(reference)) {
				return true;
			}
		}

		return inDisabledSet(reference);
	}

	// Returns the root node of a reference's loaded 3D, or null.
	void* GetRootNode(void* reference) {
		void* renderState = Field<void*>(reference, kRefr_RenderState);

		if (!renderState) {
			return nullptr;
		}

		return Field<void*>(renderState, kRenderState_RootNode);
	}

	// Reads the world bound sphere of a reference's loaded 3D into the given bounds.
	// Returns false without loaded 3D or bound.
	bool GetWorldBounds(void* reference, ReferenceBounds& bounds) {
		void* rootNode = GetRootNode(reference);

		if (!rootNode) {
			return false;
		}

		const float* bound = Field<const float*>(rootNode, kNiObject_WorldBound);

		if (!bound) {
			return false;
		}

		bounds.center = {bound[0], bound[1], bound[2]};
		bounds.radius = bound[3];

		return true;
	}

}
