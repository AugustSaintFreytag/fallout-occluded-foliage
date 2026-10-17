// Session cache of occlusion maps, one per exterior cell of the current worldspace.

#include "occlusion/map.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace sof::occlusion {

	namespace {

		// Thread: Main
		void* g_worldSpace = nullptr;
		std::unordered_map<std::uint64_t, std::unique_ptr<CellMap>> g_maps;

		std::uint64_t CellKey(std::int32_t cellX, std::int32_t cellY) {
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) << 32) | static_cast<std::uint32_t>(cellY);
		}

	}

	GridPoint ToGridPoint(float x, float y) {
		return {static_cast<std::int32_t>(std::floor(x / kGridSpacing)), static_cast<std::int32_t>(std::floor(y / kGridSpacing))};
	}

	void GridPointCenter(const GridPoint& point, float& x, float& y) {
		x = (static_cast<float>(point.x) + 0.5f) * kGridSpacing;
		y = (static_cast<float>(point.y) + 0.5f) * kGridSpacing;
	}

	// Returns the cell coordinate that holds a given grid coordinate. Rounds down for negative values.
	std::int32_t CellOf(std::int32_t gridCoordinate) {
		return gridCoordinate >= 0 ? gridCoordinate / kPointsPerSide : -((-gridCoordinate - 1) / kPointsPerSide) - 1;
	}

	// Returns the index of a grid point within its cell map, row by row.
	std::int32_t IndexInCell(const GridPoint& point) {
		const std::int32_t localX = point.x - CellOf(point.x) * kPointsPerSide;
		const std::int32_t localY = point.y - CellOf(point.y) * kPointsPerSide;

		return localY * kPointsPerSide + localX;
	}

	// Switches the maps to a given worldspace. Clears all maps if it differs from the current one.
	//
	// Thread: Main
	void SetWorldSpace(void* worldSpace) {
		if (worldSpace != g_worldSpace) {
			g_maps.clear();
			g_worldSpace = worldSpace;
		}
	}

	// Thread: Main
	void ClearMaps() {
		g_maps.clear();
		g_worldSpace = nullptr;
	}

	// Thread: Main
	CellMap& GetOrCreateCellMap(std::int32_t cellX, std::int32_t cellY) {
		std::unique_ptr<CellMap>& map = g_maps[CellKey(cellX, cellY)];

		if (!map) {
			map = std::make_unique<CellMap>();
			map->cellX = cellX;
			map->cellY = cellY;
		}

		return *map;
	}

	// Thread: Main
	CellMap* FindCellMap(std::int32_t cellX, std::int32_t cellY) {
		const auto found = g_maps.find(CellKey(cellX, cellY));

		return found == g_maps.end() ? nullptr : found->second.get();
	}

	// Stores the result for a grid point of a given map. A grid point resolves once.
	void Resolve(CellMap& map, std::int32_t index, bool isOccluded) {
		if (map.resolved.test(index)) {
			return;
		}

		map.resolved.set(index);
		map.occluded.set(index, isOccluded);
		++map.resolvedCount;
	}

	// Marks every grid point inside a given horizontal rectangle as covered, across cells.
	// Creates cell maps as needed.
	//
	// Thread: Main
	void CoverRectangle(float minX, float minY, float maxX, float maxY) {
		const GridPoint first = ToGridPoint(minX, minY);
		const GridPoint last = ToGridPoint(maxX, maxY);

		for (std::int32_t cellY = CellOf(first.y); cellY <= CellOf(last.y); ++cellY) {
			for (std::int32_t cellX = CellOf(first.x); cellX <= CellOf(last.x); ++cellX) {
				CellMap& map = GetOrCreateCellMap(cellX, cellY);

				const std::int32_t startX = std::max(first.x, cellX * kPointsPerSide) - cellX * kPointsPerSide;
				const std::int32_t endX = std::min(last.x, cellX * kPointsPerSide + kPointsPerSide - 1) - cellX * kPointsPerSide;
				const std::int32_t startY = std::max(first.y, cellY * kPointsPerSide) - cellY * kPointsPerSide;
				const std::int32_t endY = std::min(last.y, cellY * kPointsPerSide + kPointsPerSide - 1) - cellY * kPointsPerSide;

				for (std::int32_t localY = startY; localY <= endY; ++localY) {
					for (std::int32_t localX = startX; localX <= endX; ++localX) {
						map.covered.set(localY * kPointsPerSide + localX);
					}
				}
			}
		}
	}

	// Returns the state of the grid point under a given world position in the current worldspace.
	//
	// Thread: Main
	PointState Query(float x, float y) {
		const GridPoint point = ToGridPoint(x, y);
		const CellMap* map = FindCellMap(CellOf(point.x), CellOf(point.y));

		if (!map) {
			return PointState::Unknown;
		}

		const std::int32_t index = IndexInCell(point);

		if (!map->resolved.test(index)) {
			return PointState::Unknown;
		}

		return map->occluded.test(index) ? PointState::Occluded : PointState::Clear;
	}

	// Thread: Main
	void ForEachCellMap(const std::function<void(CellMap& map)>& callback) {
		for (auto& entry : g_maps) {
			callback(*entry.second);
		}
	}

}
