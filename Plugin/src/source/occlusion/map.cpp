// Session cache of occlusion maps, one per exterior cell of the current worldspace.

#include "occlusion/map.h"

#include "engine/terrain.h"

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

	// Samples the land height at every land mesh vertex of a given map's cell. Sets `hasLand` if all samples succeed.
	// Vertices on the far edges are sampled half a unit inside. This keeps the lookup in the cell.
	//
	// Thread: Main
	void SampleLand(CellMap& map) {
		const float originX = static_cast<float>(map.cellX) * kCellSize;
		const float originY = static_cast<float>(map.cellY) * kCellSize;
		const float lastOffset = kCellSize - 0.5f;

		map.hasLand = true;

		for (std::int32_t vertexY = 0; vertexY < kLandVerticesPerSide; ++vertexY) {
			for (std::int32_t vertexX = 0; vertexX < kLandVerticesPerSide; ++vertexX) {
				const float x = originX + std::min(static_cast<float>(vertexX) * kLandVertexSpacing, lastOffset);
				const float y = originY + std::min(static_cast<float>(vertexY) * kLandVertexSpacing, lastOffset);
				float& height = map.landHeights[vertexY * kLandVerticesPerSide + vertexX];

				if (!engine::GetLandHeight({x, y, 0.0f}, height)) {
					map.hasLand = false;

					return;
				}
			}
		}
	}

	// Interpolates the land height at a given world position from the samples of a given map.
	// Bilinear between the four nearest vertices. The land mesh splits each square into two triangles.
	// The result can differ from the land by a few units on uneven ground.
	// Returns false if the map has no land samples.
	//
	// Thread: Any. The samples do not change once the map is ready.
	bool LandHeightAt(const CellMap& map, float x, float y, float& height) {
		if (!map.hasLand) {
			return false;
		}

		const float localX = std::clamp((x - static_cast<float>(map.cellX) * kCellSize) / kLandVertexSpacing, 0.0f, 31.999f);
		const float localY = std::clamp((y - static_cast<float>(map.cellY) * kCellSize) / kLandVertexSpacing, 0.0f, 31.999f);
		const std::int32_t vertexX = static_cast<std::int32_t>(localX);
		const std::int32_t vertexY = static_cast<std::int32_t>(localY);
		const float fractionX = localX - static_cast<float>(vertexX);
		const float fractionY = localY - static_cast<float>(vertexY);

		const float* row = &map.landHeights[vertexY * kLandVerticesPerSide + vertexX];
		const float* nextRow = row + kLandVerticesPerSide;

		const float bottom = row[0] + (row[1] - row[0]) * fractionX;
		const float top = nextRow[0] + (nextRow[1] - nextRow[0]) * fractionX;

		height = bottom + (top - bottom) * fractionY;

		return true;
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
