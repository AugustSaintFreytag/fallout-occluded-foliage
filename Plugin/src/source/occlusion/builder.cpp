// Builds the occlusion maps around the player, nearest grid point first, under a time budget per frame.
//
// Grid points are visited in order of distance from the player's grid point (precomputed ring offsets).
// All offsets before the completed index are resolved. When the player moves by some distance,
// the completed radius shrinks by that distance and the scan resumes from there.
//
// A cell becomes ready once it is attached and every enabled occluder in it has loaded 3D, on two checks in a row.
// A cell that stays attached with some occluders still unloaded becomes ready after a timeout with the loaded ones.
// Its occluder bounds are then rasterized into the coverage of every cell they overlap.
// A grid point is probed only if its cell is ready, and every neighbour cell within the border margin is ready too.

#include "occlusion/builder.h"

#include "config/settings.h"
#include "engine/addresses.h"
#include "engine/cells.h"
#include "engine/objects.h"
#include "occlusion/map.h"
#include "probe/probe.h"
#include "utils/log.h"
#include "utils/memory.h"
#include "utils/timing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sof::occlusion {

	namespace {

		// Grid points this close to a cell edge also need the neighbour cell across that edge.
		// Covers occluders in the neighbour cell that reach over the edge.
		constexpr float kBorderMargin = 1024.0f;  // Game units
		constexpr std::int32_t kBorderMarginPoints = static_cast<std::int32_t>(kBorderMargin / kGridSpacing);

		// Cells around the player's cell that the scan can look up. Covers a loaded grid of up to 9 × 9.
		constexpr std::int32_t kWindowRadius = 4;
		constexpr std::int32_t kWindowSide = kWindowRadius * 2 + 1;

		// Grid points visited between two budget checks when no probe runs.
		constexpr std::uint32_t kVisitsPerBudgetCheck = 256;

		constexpr double kStatusIntervalSeconds = 5.0;

		// Occluders with larger bounds are left out of the coverage. Guards against broken bounds.
		// Their collision still stops probe rays where other occluders cover the grid point.
		constexpr float kMaxOccluderRadius = 8192.0f;  // Game units

		// Time after which an attached cell becomes ready even if some occluders never load 3D (missing meshes).
		constexpr double kReadinessTimeoutSeconds = 10.0;

		// References without loaded 3D listed in the log when a cell times out.
		constexpr std::size_t kMaxLoggedUnloaded = 8;

		// Offset of a grid point from the player's grid point, in grid points.
		struct Offset {
			std::int16_t x = 0;
			std::int16_t y = 0;
		};

		std::int32_t SquaredLength(const Offset& offset) {
			return static_cast<std::int32_t>(offset.x) * offset.x + static_cast<std::int32_t>(offset.y) * offset.y;
		}

		// Readiness checks of one loaded cell that is not ready yet.
		struct ReadinessCheck {
			std::int64_t firstCheck = 0;  // Counter ticks of the first check while attached
			bool passedOnce = false;
		};

		// Counters for the status line.
		struct Statistics {
			std::uint32_t frames = 0;
			std::uint32_t probes = 0;
			double totalMilliseconds = 0.0;
			double maxMilliseconds = 0.0;
		};

		// Thread: Main
		std::vector<Offset> g_offsets;
		float g_offsetRadius = 0.0f;

		void* g_worldSpace = nullptr;
		bool g_hasCenter = false;
		GridPoint g_center;
		std::size_t g_completed = 0;
		std::size_t g_cursor = 0;
		bool g_idle = false;

		std::vector<engine::LoadedCell> g_loadedCells;
		std::unordered_set<std::uint64_t> g_loadedKeys;
		std::unordered_map<std::uint64_t, ReadinessCheck> g_readinessChecks;
		std::size_t g_nextReadinessCheck = 0;

		Statistics g_statistics;
		std::int64_t g_lastStatusTime = 0;

		std::uint64_t CellKey(std::int32_t cellX, std::int32_t cellY) {
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) << 32) | static_cast<std::uint32_t>(cellY);
		}

		// Fills the ring offsets for a given radius, nearest first. Ties keep a stable row order.
		void BuildOffsets(float radius) {
			const std::int32_t radiusPoints = static_cast<std::int32_t>(std::ceil(radius / kGridSpacing));
			const std::int32_t radiusSquared = radiusPoints * radiusPoints;

			g_offsets.clear();
			g_offsets.reserve(static_cast<std::size_t>(3.15f * radiusSquared) + 16);

			for (std::int32_t y = -radiusPoints; y <= radiusPoints; ++y) {
				for (std::int32_t x = -radiusPoints; x <= radiusPoints; ++x) {
					if (x * x + y * y <= radiusSquared) {
						g_offsets.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)});
					}
				}
			}

			std::stable_sort(g_offsets.begin(), g_offsets.end(),
				[](const Offset& left, const Offset& right) { return SquaredLength(left) < SquaredLength(right); });

			g_offsetRadius = radius;
		}

		// Returns the index of the first offset at or beyond a given distance in grid points.
		std::size_t FirstOffsetAtDistance(float distance) {
			if (distance <= 0.0f) {
				return 0;
			}

			const float squared = distance * distance;

			return static_cast<std::size_t>(std::lower_bound(g_offsets.begin(), g_offsets.end(), squared,
				[](const Offset& offset, float value) { return static_cast<float>(SquaredLength(offset)) < value; })
				- g_offsets.begin());
		}

		void ResetScan() {
			g_hasCenter = false;
			g_completed = 0;
			g_cursor = 0;
			g_idle = false;
		}

		// Moves the scan center to a given grid point. Keeps the completed radius minus the distance moved.
		void MoveCenter(const GridPoint& point) {
			if (!g_hasCenter) {
				g_center = point;
				g_hasCenter = true;
				g_completed = 0;
				g_cursor = 0;
				g_idle = false;

				return;
			}

			if (point.x == g_center.x && point.y == g_center.y) {
				return;
			}

			const float moved = std::sqrt(static_cast<float>((point.x - g_center.x) * (point.x - g_center.x)
				+ (point.y - g_center.y) * (point.y - g_center.y)));

			float completedRadius = g_offsetRadius / kGridSpacing;

			if (g_completed < g_offsets.size()) {
				completedRadius = std::sqrt(static_cast<float>(SquaredLength(g_offsets[g_completed])));
			}

			g_center = point;
			g_completed = FirstOffsetAtDistance(completedRadius - moved);
			g_cursor = g_completed;
			g_idle = false;
		}

		// Records the loaded exterior cells. Marks maps of cells that left the grid as not ready.
		void UpdateLoadedCells() {
			std::vector<engine::LoadedCell> cells = engine::GetLoadedExteriorCells();
			std::unordered_set<std::uint64_t> keys;

			for (const engine::LoadedCell& cell : cells) {
				keys.insert(CellKey(cell.x, cell.y));
			}

			if (keys == g_loadedKeys) {
				g_loadedCells = std::move(cells);

				return;
			}

			ForEachCellMap([&](CellMap& map) {
				if (map.ready && !keys.count(CellKey(map.cellX, map.cellY))) {
					map.ready = false;
				}
			});

			for (auto check = g_readinessChecks.begin(); check != g_readinessChecks.end();) {
				check = keys.count(check->first) ? std::next(check) : g_readinessChecks.erase(check);
			}

			g_loadedCells = std::move(cells);
			g_loadedKeys = std::move(keys);
		}

		// Collects the world bounds of every enabled occluder in a given cell that has loaded 3D.
		// Adds each enabled occluder without loaded 3D to the given list.
		// Returns the number of enabled occluders without loaded 3D.
		std::size_t CollectOccluderBounds(void* cell, std::vector<engine::ReferenceBounds>& bounds,
			std::vector<void*>& unloadedReferences) {
			std::size_t unloaded = 0;

			for (void* reference : engine::GetCellReferences(cell)) {
				if (!probe::IsOccluder(reference) || engine::IsDisabledOrDeleted(reference)) {
					continue;
				}

				engine::ReferenceBounds referenceBounds;

				if (engine::GetWorldBounds(reference, referenceBounds)) {
					if (referenceBounds.radius > 0.0f && referenceBounds.radius <= kMaxOccluderRadius) {
						bounds.push_back(referenceBounds);
					}
				} else {
					++unloaded;
					unloadedReferences.push_back(reference);
				}
			}

			return unloaded;
		}

		// Checks one loaded cell that is not ready yet, in turn. Makes it ready on its second passed check.
		// Returns true if a cell became ready.
		bool CheckNextCellReadiness() {
			for (std::size_t attempt = 0; attempt < g_loadedCells.size(); ++attempt) {
				const engine::LoadedCell& cell = g_loadedCells[g_nextReadinessCheck++ % g_loadedCells.size()];
				CellMap* existing = FindCellMap(cell.x, cell.y);

				if (existing && existing->ready) {
					continue;
				}

				const std::uint64_t key = CellKey(cell.x, cell.y);

				if (!engine::IsAttached(cell.cell)) {
					g_readinessChecks.erase(key);

					return false;
				}

				const std::int64_t now = timing::Now();
				ReadinessCheck& check = g_readinessChecks[key];

				if (check.firstCheck == 0) {
					check.firstCheck = now;
				}

				std::vector<engine::ReferenceBounds> bounds;
				std::vector<void*> unloadedReferences;
				const std::size_t unloaded = CollectOccluderBounds(cell.cell, bounds, unloadedReferences);
				const bool timedOut =
					timing::TicksToMicroseconds(static_cast<double>(now - check.firstCheck)) >= kReadinessTimeoutSeconds * 1.0e6;

				if (unloaded > 0 && !timedOut) {
					check.passedOnce = false;

					return false;
				}

				if (unloaded == 0 && !check.passedOnce) {
					check.passedOnce = true;

					return false;
				}

				g_readinessChecks.erase(key);

				for (const engine::ReferenceBounds& reference : bounds) {
					CoverRectangle(reference.center.x - reference.radius, reference.center.y - reference.radius,
						reference.center.x + reference.radius, reference.center.y + reference.radius);
				}

				CellMap& map = GetOrCreateCellMap(cell.x, cell.y);
				map.ready = true;

				if (config::Get().debug.logMap) {
					SOF_LOG("[Map] Cell (%d, %d) ready with %zu occluders, %zu without 3D after the timeout. %zu of %d grid points "
							"resolved before.",
						cell.x, cell.y, bounds.size(), unloaded, static_cast<std::size_t>(map.resolvedCount), kPointsPerCell);

					for (std::size_t index = 0; index < unloadedReferences.size() && index < kMaxLoggedUnloaded; ++index) {
						void* reference = unloadedReferences[index];
						void* baseForm = engine::GetBaseForm(reference);

						SOF_LOG("[Map]   Without 3D: Ref=%08X '%s' Flags=%08X Base=%s %08X '%s' Model=\"%s\"", engine::GetFormID(reference),
							engine::GetEditorID(reference), mem::Field<std::uint32_t>(reference, engine::kForm_Flags),
							engine::FormTypeName(engine::GetFormType(baseForm)), engine::GetFormID(baseForm),
							engine::GetEditorID(baseForm), engine::GetModelPath(baseForm));
					}
				}

				return true;
			}

			return false;
		}

		// Returns the map of a given cell from the window around the player's cell, or null outside the window.
		CellMap* WindowMap(CellMap* const* window, std::int32_t centerCellX, std::int32_t centerCellY, std::int32_t cellX,
			std::int32_t cellY) {
			const std::int32_t windowX = cellX - centerCellX + kWindowRadius;
			const std::int32_t windowY = cellY - centerCellY + kWindowRadius;

			if (windowX < 0 || windowY < 0 || windowX >= kWindowSide || windowY >= kWindowSide) {
				return nullptr;
			}

			return window[windowY * kWindowSide + windowX];
		}

		bool IsReady(const CellMap* map) {
			return map && map->ready;
		}

		// Checks if every neighbour cell within the border margin of a given grid point is ready.
		bool AreNeighboursReady(CellMap* const* window, std::int32_t centerCellX, std::int32_t centerCellY,
			const GridPoint& point, std::int32_t cellX, std::int32_t cellY) {
			const std::int32_t localX = point.x - cellX * kPointsPerSide;
			const std::int32_t localY = point.y - cellY * kPointsPerSide;

			const std::int32_t stepX = localX < kBorderMarginPoints ? -1 : (localX >= kPointsPerSide - kBorderMarginPoints ? 1 : 0);
			const std::int32_t stepY = localY < kBorderMarginPoints ? -1 : (localY >= kPointsPerSide - kBorderMarginPoints ? 1 : 0);

			if (stepX != 0 && !IsReady(WindowMap(window, centerCellX, centerCellY, cellX + stepX, cellY))) {
				return false;
			}

			if (stepY != 0 && !IsReady(WindowMap(window, centerCellX, centerCellY, cellX, cellY + stepY))) {
				return false;
			}

			if (stepX != 0 && stepY != 0 && !IsReady(WindowMap(window, centerCellX, centerCellY, cellX + stepX, cellY + stepY))) {
				return false;
			}

			return true;
		}

		void LogCellComplete(CellMap& map) {
			map.completeLogged = true;

			if (!config::Get().debug.logMap) {
				return;
			}

			SOF_LOG("[Map] Cell (%d, %d) complete: %zu covered, %zu occluded grid points, %u probes, %u casts, %.2f ms probing.",
				map.cellX, map.cellY, map.covered.count(), map.occluded.count(), map.probeCount, map.castCount,
				map.probeMicroseconds / 1000.0);
		}

		// Visits grid points from the cursor outward until the budget runs out. Returns the number of probes.
		std::uint32_t Scan(double budgetMicroseconds, std::int64_t startTime) {
			const std::int32_t centerCellX = CellOf(g_center.x);
			const std::int32_t centerCellY = CellOf(g_center.y);

			CellMap* window[kWindowSide * kWindowSide] = {};

			for (std::int32_t windowY = 0; windowY < kWindowSide; ++windowY) {
				for (std::int32_t windowX = 0; windowX < kWindowSide; ++windowX) {
					window[windowY * kWindowSide + windowX] =
						FindCellMap(centerCellX + windowX - kWindowRadius, centerCellY + windowY - kWindowRadius);
				}
			}

			const float probeHeight = config::Get().probe.probeHeight;
			std::uint32_t probes = 0;
			std::uint32_t visits = 0;

			while (g_cursor < g_offsets.size()) {
				if (++visits % kVisitsPerBudgetCheck == 0
					&& timing::TicksToMicroseconds(static_cast<double>(timing::Now() - startTime)) >= budgetMicroseconds) {
					break;
				}

				const Offset& offset = g_offsets[g_cursor];
				const GridPoint point{g_center.x + offset.x, g_center.y + offset.y};
				const std::int32_t cellX = CellOf(point.x);
				const std::int32_t cellY = CellOf(point.y);
				CellMap* map = WindowMap(window, centerCellX, centerCellY, cellX, cellY);

				bool resolved = false;

				if (IsReady(map)) {
					const std::int32_t index = IndexInCell(point);

					if (map->resolved.test(index)) {
						resolved = true;
					} else if (AreNeighboursReady(window, centerCellX, centerCellY, point, cellX, cellY)) {
						if (!map->covered.test(index)) {
							Resolve(*map, index, false);
							resolved = true;
						} else {
							float x = 0.0f;
							float y = 0.0f;
							GridPointCenter(point, x, y);

							const std::int64_t probeStart = timing::Now();
							const probe::ProbeResult result = probe::ProbePoint(x, y, probeHeight);
							const double probeMicroseconds = timing::TicksToMicroseconds(static_cast<double>(timing::Now() - probeStart));

							++probes;
							++map->probeCount;
							map->castCount += result.casts;
							map->probeMicroseconds += probeMicroseconds;

							if (result.verdict != probe::Verdict::NoTerrain) {
								Resolve(*map, index, result.verdict == probe::Verdict::Occluded);
								resolved = true;
							}
						}

						if (map->resolvedCount == kPointsPerCell && !map->completeLogged) {
							LogCellComplete(*map);
						}
					}
				}

				if (resolved && g_cursor == g_completed) {
					++g_completed;
				}

				++g_cursor;

				if (probes > 0 && timing::TicksToMicroseconds(static_cast<double>(timing::Now() - startTime)) >= budgetMicroseconds) {
					break;
				}
			}

			if (g_cursor >= g_offsets.size()) {
				g_idle = true;
			}

			return probes;
		}

		void LogStatus(std::int64_t now) {
			if (g_lastStatusTime == 0) {
				g_lastStatusTime = now;

				return;
			}

			if (timing::TicksToMicroseconds(static_cast<double>(now - g_lastStatusTime)) < kStatusIntervalSeconds * 1.0e6) {
				return;
			}

			std::size_t readyCells = 0;
			ForEachCellMap([&](CellMap& map) { readyCells += map.ready ? 1 : 0; });

			float completedRadius = g_offsetRadius;

			if (g_completed < g_offsets.size()) {
				completedRadius = std::sqrt(static_cast<float>(SquaredLength(g_offsets[g_completed]))) * kGridSpacing;
			}

			SOF_LOG("[Map] Status: completed radius %.0f of %.0f, %s, %zu of %zu loaded cells ready. %u probes in %u frames, "
					"%.3f ms average, %.3f ms max per frame.",
				completedRadius, g_offsetRadius, g_idle ? "idle" : "scanning", readyCells, g_loadedCells.size(),
				g_statistics.probes, g_statistics.frames, g_statistics.totalMilliseconds / std::max<std::uint32_t>(g_statistics.frames, 1),
				g_statistics.maxMilliseconds);

			g_statistics = {};
			g_lastStatusTime = now;
		}

	}

	// Advances the occlusion maps by one frame: tracks loaded cells, checks one cell for readiness and probes
	// grid points outward from the player within the time budget.
	//
	// Thread: Main (each frame)
	void UpdateBuilder() {
		const std::int64_t startTime = timing::Now();
		const config::Settings& settings = config::Get();

		void* player = engine::GetPlayer();
		void* cell = player ? engine::GetParentCell(player) : nullptr;

		if (!cell || engine::IsInterior(cell)) {
			return;
		}

		void* worldSpace = engine::GetWorldSpace(cell);

		if (worldSpace != g_worldSpace) {
			SetWorldSpace(worldSpace);
			g_worldSpace = worldSpace;
			g_loadedKeys.clear();
			g_readinessChecks.clear();
			ResetScan();
		}

		if (g_offsets.empty() || g_offsetRadius != settings.probe.radius) {
			BuildOffsets(settings.probe.radius);
			ResetScan();
		}

		UpdateLoadedCells();

		const bool cellBecameReady = !g_loadedCells.empty() && CheckNextCellReadiness();
		const Vector3 position = engine::GetPosition(player);

		MoveCenter(ToGridPoint(position.x, position.y));

		if (cellBecameReady) {
			g_cursor = g_completed;
			g_idle = false;
		}

		std::uint32_t probes = 0;

		if (!g_idle) {
			probes = Scan(settings.probe.budget * 1000.0, startTime);
		}

		const double milliseconds = timing::TicksToMilliseconds(static_cast<double>(timing::Now() - startTime));

		++g_statistics.frames;
		g_statistics.probes += probes;
		g_statistics.totalMilliseconds += milliseconds;
		g_statistics.maxMilliseconds = std::max(g_statistics.maxMilliseconds, milliseconds);

		if (settings.debug.logMap) {
			LogStatus(timing::Now());
		}
	}

	// Clears all occlusion maps and the scan state. Called on game load and on exit to the main menu.
	//
	// Thread: Main
	void ResetBuilder() {
		ClearMaps();
		ResetScan();

		g_worldSpace = nullptr;
		g_loadedCells.clear();
		g_loadedKeys.clear();
		g_readinessChecks.clear();
		g_statistics = {};
		g_lastStatusTime = 0;
	}

}
