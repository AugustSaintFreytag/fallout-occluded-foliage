#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>

namespace sof::occlusion {

	// Library

	constexpr float kCellSize = 4096.0f;  // Game units
	constexpr float kGridSpacing = 32.0f;  // Game units, ~45 cm
	constexpr std::int32_t kPointsPerSide = 128;  // Grid points along one side of a cell
	constexpr std::int32_t kPointsPerCell = kPointsPerSide * kPointsPerSide;

	constexpr std::int32_t kLandVerticesPerSide = 33;  // Vertices of the land mesh along one side of a cell
	constexpr float kLandVertexSpacing = 128.0f;  // Game units

	// Global index of a grid point. The point sits at the centre of its 32-unit square.
	struct GridPoint {
		std::int32_t x = 0;
		std::int32_t y = 0;
	};

	// Result of a map lookup at one grid point.
	enum class PointState : std::uint8_t {
		Unknown,
		Clear,
		Occluded,
	};

	// Occlusion state of the grid points of one exterior cell.
	//
	// A covered grid point lies inside the horizontal bounds of an occluder and wants to be probed.
	// Grid points outside of an occluder boundary get skipped and marked as clear.
	//
	// The land heights are sampled at the land mesh vertices when the cell becomes ready.
	// They let the async worker place probe rays without engine calls.
	struct CellMap {
		std::int32_t cellX = 0;
		std::int32_t cellY = 0;

		std::bitset<kPointsPerCell> covered;
		std::bitset<kPointsPerCell> resolved;
		std::bitset<kPointsPerCell> occluded;
		std::bitset<kPointsPerCell> pending;

		std::array<float, kLandVerticesPerSide * kLandVerticesPerSide> landHeights{};  // Game units
		bool hasLand = false;

		std::uint32_t resolvedCount = 0;
		std::uint32_t probeCount = 0;
		std::uint32_t castCount = 0;
		double probeMicroseconds = 0.0;
		std::int64_t readyTime = 0;  // Counter ticks

		// Counts the grid points resolved as occluded. The grass hider compares it to see which cells changed.
		std::uint32_t occludedVersion = 0;

		// Unique per map created in the session. Tells a map apart from an earlier one of the same cell.
		std::uint32_t serial = 0;

		// Cell is loaded and its occluders have been rasterized into the coverage. 
		// Gets cleared when the cell unloads.
		bool ready = false;
		bool completeLogged = false;
	};

	// Lifecycle

	GridPoint ToGridPoint(float x, float y);
	void GridPointCenter(const GridPoint& point, float& x, float& y);
	std::int32_t CellOf(std::int32_t gridCoordinate);
	std::int32_t IndexInCell(const GridPoint& point);

	void SetWorldSpace(void* worldSpace);
	void ClearMaps();

	CellMap& GetOrCreateCellMap(std::int32_t cellX, std::int32_t cellY);
	CellMap* FindCellMap(std::int32_t cellX, std::int32_t cellY);

	void Resolve(CellMap& map, std::int32_t index, bool isOccluded);
	std::uint32_t GetOcclusionVersion();

	void SampleLand(CellMap& map);
	bool LandHeightAt(const CellMap& map, float x, float y, float& height);
	void CoverRectangle(float minX, float minY, float maxX, float maxY);

	PointState Query(float x, float y);

	void ForEachCellMap(const std::function<void(CellMap& map)>& callback);

}
