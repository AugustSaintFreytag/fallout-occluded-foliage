// Builds the occlusion maps around the player, nearest grid point first, under a time budget per frame.
//
// Grid points are visited in order of distance from the player's grid point (precomputed ring offsets).
// All offsets before the completed index are resolved. When the player moves by some distance,
// the completed radius shrinks by that distance and the scan resumes from there.
//
// A cell is considered ready once it is attached and every enabled occluder in it has loaded meshs (3D).
// Occluder bounds get rasterized into the coverage of every cell they overlap.
// A grid point is probed only if the containing and nearby cells are all ready.

#include "occlusion/builder.h"

#include "config/settings.h"
#include "engine/addresses.h"
#include "engine/cells.h"
#include "engine/havok.h"
#include "engine/objects.h"
#include "occlusion/map.h"
#include "occlusion/worker.h"
#include "probe/probe.h"
#include "utils/formatting.h"
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
			std::uint32_t probes = 0;  // On the main thread
			std::uint32_t asyncFrames = 0;  // Frames with the worker running
			std::uint32_t droppedOutcomes = 0;
			std::uint32_t retriedPoints = 0;

			double totalMilliseconds = 0.0;
			double maxMilliseconds = 0.0;
		};

		// A stretch of frames in which the scan had work.
		struct ScanRun {
			bool active = false;
			std::int64_t startTime = 0;  // Counter ticks
			std::size_t probes = 0;  // Main thread and worker
		};

		// Frames the loaded cell set must stay the same before the worker runs.
		// Cells that attach likely add their collision to the Havok world without the global lock. Not verified.
		constexpr std::uint32_t kWorkerSettleFrames = 30;

		// Scan runs with fewer probes are not logged.
		constexpr std::size_t kMinLoggedScanProbes = 1000;

		// Queued tasks per probe the worker finished in the last frame. Keeps the queue filled for about two frames.
		constexpr std::size_t kQueueFramesAhead = 2;
		constexpr std::size_t kMinQueuedTasks = 64;
		constexpr std::size_t kMaxQueuedTasks = 16384;

		// Thread: Main
		std::vector<Offset> g_offsets;
		float g_offsetRadius = 0.0f;

		void* g_worldSpace = nullptr;
		bool g_hasCenter = false;
		GridPoint g_center;
		std::size_t g_completed = 0;
		std::size_t g_cursor = 0;
		bool g_idle = false;

		bool g_workerActive = false;
		std::uint32_t g_framesSinceCellChange = 0;
		std::size_t g_lastWorkerProbes = 0;
		std::vector<ProbeOutcome> g_outcomes;
		std::vector<ProbeTask> g_tasks;

		std::vector<engine::LoadedCell> g_loadedCells;
		std::unordered_set<std::uint64_t> g_loadedKeys;
		std::unordered_map<std::uint64_t, ReadinessCheck> g_readinessChecks;
		std::size_t g_nextReadinessCheck = 0;

		Statistics g_statistics;
		ScanRun g_scanRun;
		std::int64_t g_lastStatusTime = 0;
		std::size_t g_lastCoveredResolved = 0;
		bool g_idleLogged = false;
		std::uint32_t g_lastPickFaults = 0;

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
		// Returns true if the set of loaded cells changed.
		bool UpdateLoadedCells() {
			std::vector<engine::LoadedCell> cells = engine::GetLoadedExteriorCells();
			std::unordered_set<std::uint64_t> keys;

			for (const engine::LoadedCell& cell : cells) {
				keys.insert(CellKey(cell.x, cell.y));
			}

			if (keys == g_loadedKeys) {
				g_loadedCells = std::move(cells);

				return false;
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

			return true;
		}

		// Collects the world bounds of every enabled occluder in a given cell that has loaded 3D.
		// Adds each enabled occluder without loaded 3D to the given list.
		// Occluders with a marker model are left out. Their 3D never loads, so a cell would wait for the timeout.
		// Returns the number of enabled occluders without loaded 3D.
		std::size_t CollectOccluderBounds(void* cell, std::vector<engine::ReferenceBounds>& bounds,
			std::vector<void*>& unloadedReferences) {
			std::size_t unloaded = 0;

			for (void* reference : engine::GetCellReferences(cell)) {
				if (!probe::IsOccluder(reference) || engine::IsDisabledOrDeleted(reference)
					|| engine::HasMarkerModel(engine::GetBaseForm(reference))) {
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

		void LogCellPrepared(const CellMap& map, std::size_t occluders, std::size_t unloaded) {
			char details[160] = "";
			std::size_t length = 0;

			if (unloaded > 0) {
				length += std::snprintf(details + length, sizeof(details) - length, " %zu occluder%s without 3D after %.0fs.",
					unloaded, format::Plural(unloaded), kReadinessTimeoutSeconds);
			}

			if (!map.hasLand) {
				length += std::snprintf(details + length, sizeof(details) - length, " No land.");
			}

			if (map.resolvedCount > 0) {
				std::snprintf(details + length, sizeof(details) - length, " Kept %s resolved grid points.",
					format::Count(map.resolvedCount).c_str());
			}

			// SOF_LOG("[Map] Prepared cell (%d, %d) with %zu occluder%s.%s", map.cellX, map.cellY, occluders, format::Plural(occluders), details);
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
					CoverRectangle(
						reference.center.x - reference.radius, 
						reference.center.y - reference.radius,
						reference.center.x + reference.radius, 
						reference.center.y + reference.radius
					);
				}

				CellMap& map = GetOrCreateCellMap(cell.x, cell.y);
				SampleLand(map);
				map.ready = true;
				map.readyTime = now;

				if (config::Get().debug.logMap) {
					LogCellPrepared(map, bounds.size(), unloaded);

					for (std::size_t index = 0; index < unloadedReferences.size() && index < kMaxLoggedUnloaded; ++index) {
						void* reference = unloadedReferences[index];
						void* baseForm = engine::GetBaseForm(reference);

						SOF_LOG("[Map]   Without 3D: '%s' (%08X, flags %08X), base %s '%s' (%08X), \"%s\".",
							engine::GetEditorID(reference), engine::GetFormID(reference),
							mem::Field<std::uint32_t>(reference, engine::kForm_Flags),
							engine::FormTypeName(engine::GetFormType(baseForm)), engine::GetEditorID(baseForm),
							engine::GetFormID(baseForm), engine::GetModelPath(baseForm));
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

		// Fills the window of cell maps around a given center cell. Cells without a map stay null.
		void FillWindow(CellMap** window, std::int32_t centerCellX, std::int32_t centerCellY) {
			for (std::int32_t windowY = 0; windowY < kWindowSide; ++windowY) {
				for (std::int32_t windowX = 0; windowX < kWindowSide; ++windowX) {
					window[windowY * kWindowSide + windowX] =
						FindCellMap(centerCellX + windowX - kWindowRadius, centerCellY + windowY - kWindowRadius);
				}
			}
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

			const std::size_t covered = map.covered.count();
			const std::size_t occluded = map.occluded.count();

			const double seconds = timing::TicksToMicroseconds(static_cast<double>(timing::Now() - map.readyTime)) / 1.0e6;
			const double coveredFraction = covered ? 100.0 * occluded / covered : 0.0;

			SOF_LOG(
				"[Map] Completed cell (%d, %d): %s/%s covered grid points occluded (%.0f%%) (took %s).", 
				map.cellX, 
				map.cellY, 
				format::Count(occluded).c_str(), 
				format::Count(covered).c_str(), 
				coveredFraction, 
				format::Duration(seconds).c_str()
			);

			SOF_LOG(
				"[Map] Performed %s probes, %s casts, %.1fms probing, %.1fµs per probe.", 
				format::Count(map.probeCount).c_str(),
				format::Count(map.castCount).c_str(), 
				map.probeMicroseconds / 1000.0,
				map.probeMicroseconds / std::max<std::uint32_t>(map.probeCount, 1)
			);
		}

		// Moves the completed index and the cursor back to a given grid point of a given cell, so the scan visits it again.
		void Rewind(std::int32_t cellX, std::int32_t cellY, std::int32_t index) {
			if (!g_hasCenter) {
				return;
			}

			const std::int32_t pointX = cellX * kPointsPerSide + index % kPointsPerSide;
			const std::int32_t pointY = cellY * kPointsPerSide + index / kPointsPerSide;
			
			const float distance = std::sqrt(static_cast<float>(
				(pointX - g_center.x) * (pointX - g_center.x) + (pointY - g_center.y) * (pointY - g_center.y)
			));

			g_completed = std::min(g_completed, FirstOffsetAtDistance(distance));
			g_cursor = g_completed;
			g_idle = false;
		}

		// Clears the pending mark of a given task's grid point and rewinds the scan to it.
		void ReleaseTask(std::int32_t cellX, std::int32_t cellY, std::int32_t index) {
			if (CellMap* map = FindCellMap(cellX, cellY)) {
				map->pending.reset(index);
			}

			Rewind(cellX, cellY, index);
			++g_statistics.retriedPoints;
		}

		// Resolves the grid points of the given outcomes.
		// Releases faulted outcomes and those from cells that aren't ready.
		// Returns the number of grid points resolved.
		std::size_t ApplyOutcomes(const std::vector<ProbeOutcome>& outcomes) {
			std::size_t applied = 0;

			for (const ProbeOutcome& outcome : outcomes) {
				CellMap* map = FindCellMap(outcome.cellX, outcome.cellY);

				if (!map || !map->ready || outcome.faulted) {
					ReleaseTask(outcome.cellX, outcome.cellY, outcome.index);
					continue;
				}

				map->pending.reset(outcome.index);
				Resolve(*map, outcome.index, outcome.verdict == probe::Verdict::Occluded);
				++applied;

				++map->probeCount;
				map->castCount += outcome.casts;
				map->probeMicroseconds += outcome.microseconds;

				if (map->resolvedCount == kPointsPerCell && !map->completeLogged) {
					LogCellComplete(*map);
				}
			}

			return applied;
		}

		// Pauses the worker and releases its queued tasks and every outcome not applied yet.
		void PauseAndRelease() {
			for (const ProbeTask& task : PauseWorker()) {
				ReleaseTask(task.cellX, task.cellY, task.index);
			}

			g_outcomes.clear();
			CollectOutcomes(g_outcomes);

			for (const ProbeOutcome& outcome : g_outcomes) {
				ReleaseTask(outcome.cellX, outcome.cellY, outcome.index);
			}

			g_statistics.droppedOutcomes += static_cast<std::uint32_t>(g_outcomes.size());
			g_outcomes.clear();
			g_workerActive = false;
		}

		// Visits grid points from the cursor outward until the budget runs out.
		// Returns the number of probes.
		//
		// Probes covered grid points on the main thread.
		// With a task limit above 0, queues them as tasks up to that limit instead and marks them pending.
		std::uint32_t Scan(double budgetMicroseconds, std::int64_t startTime, void* castCell, std::size_t taskLimit) {
			const std::int32_t centerCellX = CellOf(g_center.x);
			const std::int32_t centerCellY = CellOf(g_center.y);

			CellMap* window[kWindowSide * kWindowSide] = {};
			FillWindow(window, centerCellX, centerCellY);

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

				// A pending grid point counts as resolved for the completed index. Its outcome follows,
				// and a faulted or dropped outcome rewinds the scan to it.
				bool resolved = false;
				bool queueFull = false;

				if (IsReady(map)) {
					const std::int32_t index = IndexInCell(point);
					float x = 0.0f;
					float y = 0.0f;
					float terrainHeight = 0.0f;
					GridPointCenter(point, x, y);

					if (map->resolved.test(index) || map->pending.test(index)) {
						resolved = true;
					} else if (AreNeighboursReady(window, centerCellX, centerCellY, point, cellX, cellY)) {
						if (!map->covered.test(index) || !LandHeightAt(*map, x, y, terrainHeight)) {
							Resolve(*map, index, false);
							resolved = true;
						} else if (taskLimit > 0) {
							if (g_tasks.size() < taskLimit) {
								map->pending.set(index);
								g_tasks.push_back({cellX, cellY, index, x, y, terrainHeight});
								resolved = true;
							} else {
								queueFull = true;
							}
						} else {
							const std::int64_t probeStart = timing::Now();
							const probe::ProbeResult result = probe::ProbePoint(castCell, x, y, terrainHeight, probeHeight);
							const double probeMicroseconds = timing::TicksToMicroseconds(static_cast<double>(timing::Now() - probeStart));

							++probes;
							++map->probeCount;
							map->castCount += result.casts;
							map->probeMicroseconds += probeMicroseconds;

							if (!result.faulted) {
								Resolve(*map, index, result.verdict == probe::Verdict::Occluded);
								resolved = true;
							}
						}

						if (map->resolvedCount == kPointsPerCell && !map->completeLogged) {
							LogCellComplete(*map);
						}
					}
				}

				if (queueFull) {
					break;
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

		// Logs a scan run when it ends: from the first frame with probes to the first frame without work.
		// Frames that wait for cells before the first probe do not count.
		// Runs below `kMinLoggedScanProbes` probes are not logged. Player movement causes many of them.
		void TrackScanRun(bool scanning, std::size_t probes, std::int64_t now) {
			if (scanning && probes > 0 && !g_scanRun.active) {
				g_scanRun = {};
				g_scanRun.active = true;
				g_scanRun.startTime = now;
			}

			g_scanRun.probes += probes;

			if (scanning || !g_scanRun.active) {
				return;
			}

			g_scanRun.active = false;

			if (g_scanRun.probes < kMinLoggedScanProbes) {
				return;
			}

			const double seconds = timing::TicksToMicroseconds(static_cast<double>(now - g_scanRun.startTime)) / 1.0e6;

			SOF_LOG("[Map] Finished scan in %.2f s: %s probes, %s per second.", seconds, format::Count(g_scanRun.probes).c_str(),
				format::Count(static_cast<std::uint64_t>(g_scanRun.probes / std::max(seconds, 0.001))).c_str());
		}

		// Covered grid points within the probe radius that the scan can reach.
		struct ScanProgress {
			std::size_t reachable = 0;
			std::size_t resolved = 0;
		};

		// Counts the covered grid points within the probe radius whose cell and neighbour cells are ready.
		// Visits every ring offset. Runs only for the progress block.
		ScanProgress CountScanProgress() {
			ScanProgress progress;

			if (!g_hasCenter) {
				return progress;
			}

			const std::int32_t centerCellX = CellOf(g_center.x);
			const std::int32_t centerCellY = CellOf(g_center.y);

			CellMap* window[kWindowSide * kWindowSide] = {};
			FillWindow(window, centerCellX, centerCellY);

			for (const Offset& offset : g_offsets) {
				const GridPoint point{g_center.x + offset.x, g_center.y + offset.y};
				const std::int32_t cellX = CellOf(point.x);
				const std::int32_t cellY = CellOf(point.y);
				const CellMap* map = WindowMap(window, centerCellX, centerCellY, cellX, cellY);

				if (!IsReady(map)) {
					continue;
				}

				const std::int32_t index = IndexInCell(point);

				if (!map->covered.test(index) || !AreNeighboursReady(window, centerCellX, centerCellY, point, cellX, cellY)) {
					continue;
				}

				++progress.reachable;
				progress.resolved += map->resolved.test(index) ? 1 : 0;
			}

			return progress;
		}

		// Logs the build progress over the ready cells and the cost of the last interval, every few seconds.
		// Stays quiet while the scan is idle and nothing changed since the last progress block.
		void LogStatus(std::int64_t now) {
			if (g_lastStatusTime == 0) {
				g_lastStatusTime = now;

				return;
			}

			const double intervalSeconds = timing::TicksToMicroseconds(static_cast<double>(now - g_lastStatusTime)) / 1.0e6;

			if (intervalSeconds < kStatusIntervalSeconds) {
				return;
			}

			const WorkerStatistics worker = TakeWorkerStatistics();

			std::size_t readyCells = 0;
			std::size_t completeCells = 0;

			ForEachCellMap([&](CellMap& map) {
				readyCells += map.ready ? 1 : 0;
				completeCells += map.ready && map.resolvedCount == kPointsPerCell ? 1 : 0;
			});

			const ScanProgress progress = CountScanProgress();
			const std::size_t coveredResolved = progress.resolved;
			const std::size_t covered = progress.reachable;
			const std::uint32_t pickFaults = engine::GetPickFaultCount();
			const std::uint32_t newPickFaults = pickFaults - g_lastPickFaults;

			const bool worked = g_statistics.probes > 0 || worker.probes > 0 || coveredResolved != g_lastCoveredResolved;

			if (g_idle && !worked && g_idleLogged) {
				g_statistics = {};
				g_lastStatusTime = now;
				g_lastPickFaults = pickFaults;

				return;
			}

			const double percent = covered ? 100.0 * coveredResolved / covered : 100.0;

			SOF_LOG("[Map] Progress %.1f%%: %s of %s covered grid points within %s units. %zu of %zu loaded cell%s ready, "
					"%zu complete.", percent,
				format::Count(coveredResolved).c_str(), format::Count(covered).c_str(),
				format::Count(static_cast<std::uint64_t>(g_offsetRadius)).c_str(), readyCells, g_loadedCells.size(),
				format::Plural(g_loadedCells.size()), completeCells);

			float completedRadius = g_offsetRadius;

			if (g_completed < g_offsets.size()) {
				completedRadius = std::sqrt(static_cast<float>(SquaredLength(g_offsets[g_completed]))) * kGridSpacing;
			}

			// Resolved grid points drop when cells unload. The rate then counts as 0 for the interval.
			const double rate = coveredResolved > g_lastCoveredResolved
				? (coveredResolved - g_lastCoveredResolved) / intervalSeconds : 0.0;

			if (g_idle) {
				SOF_LOG("[Map] Scan idle at radius %s.", format::Count(static_cast<std::uint64_t>(g_offsetRadius)).c_str());
			} else if (rate > 0.0) {
				SOF_LOG("[Map] Scan at radius %s of %s, %s grid points per second (ETA: %s).",
					format::Count(static_cast<std::uint64_t>(completedRadius)).c_str(),
					format::Count(static_cast<std::uint64_t>(g_offsetRadius)).c_str(),
					format::Count(static_cast<std::uint64_t>(rate)).c_str(),
					format::Duration((covered - coveredResolved) / rate).c_str());
			} else {
				SOF_LOG("[Map] Scan at radius %s of %s, waiting for cells.",
					format::Count(static_cast<std::uint64_t>(completedRadius)).c_str(),
					format::Count(static_cast<std::uint64_t>(g_offsetRadius)).c_str());
			}

			SOF_LOG("[Map] Main thread: %.3fms per frame, %.3fms max, %s probes in %s frames.",
				g_statistics.totalMilliseconds / std::max<std::uint32_t>(g_statistics.frames, 1), g_statistics.maxMilliseconds,
				format::Count(g_statistics.probes).c_str(), format::Count(g_statistics.frames).c_str());

			if (config::Get().probe.async && worker.probes == 0) {
				SOF_LOG("[Map] Worker: no probes.");
			} else if (config::Get().probe.async) {
				const double workerFrames = std::max<std::uint32_t>(worker.frames, 1);
				const double probes = std::max<std::uint32_t>(worker.probes, 1);
				const double bursts = std::max<std::uint32_t>(worker.bursts, 1);

				SOF_LOG("[Map] Worker: %.3fms per frame, %s probes in %s of %s frames, %.1fµs and %.1f casts per probe, "
						"%u fault%s.",
					worker.busyMilliseconds / workerFrames, format::Count(worker.probes).c_str(),
					format::Count(worker.frames).c_str(), format::Count(g_statistics.asyncFrames).c_str(),
					worker.busyMilliseconds * 1000.0 / probes, worker.casts / probes, worker.faults, format::Plural(worker.faults));

				SOF_LOG("[Map] Havok lock: %s bursts, %.3fms wait on average, %.3f ms max, %.3f ms longest hold, "
						"%s tasks queued.",
					format::Count(worker.bursts).c_str(), worker.lockWaitMilliseconds / bursts, worker.maxLockWaitMilliseconds,
					worker.maxBurstMilliseconds, format::Count(GetQueuedTaskCount()).c_str());
			}

			if (g_statistics.retriedPoints > 0 || g_statistics.droppedOutcomes > 0 || newPickFaults > 0) {
				SOF_LOG("[Map] Retried %s grid points, %s outcomes dropped, %s pick fault%s. %s pick faults since load.",
					format::Count(g_statistics.retriedPoints).c_str(), format::Count(g_statistics.droppedOutcomes).c_str(),
					format::Count(newPickFaults).c_str(), format::Plural(newPickFaults), format::Count(pickFaults).c_str());
			}

			g_idleLogged = g_idle;
			g_lastCoveredResolved = coveredResolved;
			g_lastPickFaults = pickFaults;
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
			if (g_workerActive) {
				PauseAndRelease();
			}

			BuildOffsets(settings.probe.radius);
			ResetScan();
		}

		const bool loadedCellsChanged = UpdateLoadedCells();

		// Results from a frame with cell changes may come from a half attached or detached world.
		if (g_workerActive && loadedCellsChanged) {
			PauseAndRelease();
		}

		const bool cellBecameReady = !g_loadedCells.empty() && CheckNextCellReadiness();
		const Vector3 position = engine::GetPosition(player);

		MoveCenter(ToGridPoint(position.x, position.y));

		std::size_t appliedOutcomes = 0;

		if (g_workerActive) {
			g_outcomes.clear();
			CollectOutcomes(g_outcomes);
			appliedOutcomes = ApplyOutcomes(g_outcomes);
			g_lastWorkerProbes = g_outcomes.size();
		}

		g_framesSinceCellChange = loadedCellsChanged ? 0 : std::min(g_framesSinceCellChange + 1, kWorkerSettleFrames);

		// The scan queues only grid points whose cell and border neighbours are ready.
		// Cells still waiting for occluder 3D do not hold the worker back.
		const bool workerAllowed = settings.probe.async && !g_loadedCells.empty()
			&& g_framesSinceCellChange >= kWorkerSettleFrames;

		if (g_workerActive && !workerAllowed) {
			PauseAndRelease();
		}

		if (cellBecameReady) {
			g_cursor = g_completed;
			g_idle = false;
		}

		std::uint32_t probes = 0;

		if (workerAllowed) {
			StartWorker();

			const std::size_t target = std::clamp(std::max(kMinQueuedTasks, std::max<std::size_t>(settings.probe.batchSize * 4,
				g_lastWorkerProbes * kQueueFramesAhead)), kMinQueuedTasks, kMaxQueuedTasks);
			const std::size_t queued = GetQueuedTaskCount();

			if (!g_idle && queued < target) {
				g_tasks.clear();
				Scan(settings.probe.syncBudget * 1000.0, startTime, cell, target - queued);
				SubmitTasks(g_tasks);
			}

			BeginWorkerFrame(cell, settings.probe.asyncBudget, settings.probe.batchSize, settings.probe.probeHeight);
			g_workerActive = true;
			++g_statistics.asyncFrames;
		} else if (!g_idle) {
			probes = Scan(settings.probe.syncBudget * 1000.0, startTime, cell, 0);
		}

		const double milliseconds = timing::TicksToMilliseconds(static_cast<double>(timing::Now() - startTime));

		++g_statistics.frames;
		g_statistics.probes += probes;
		g_statistics.totalMilliseconds += milliseconds;
		g_statistics.maxMilliseconds = std::max(g_statistics.maxMilliseconds, milliseconds);

		if (settings.debug.logMap) {
			const std::int64_t now = timing::Now();
			const bool scanning = !g_idle || appliedOutcomes > 0 || (g_workerActive && GetQueuedTaskCount() > 0);

			TrackScanRun(scanning, probes + appliedOutcomes, now);
			LogStatus(now);
		}
	}

	// Builds the ring offsets for the configured radius. The sort takes several milliseconds,
	// so it runs at load instead of on the first frame. A changed radius rebuilds them on the next frame.
	//
	// Thread: Main (load time only)
	void InitializeBuilder() {
		const std::int64_t startTime = timing::Now();

		BuildOffsets(config::Get().probe.radius);

		SOF_LOG("[Map] Built %s ring offsets for radius %s in %.1f ms.", format::Count(g_offsets.size()).c_str(),
			format::Count(static_cast<std::uint64_t>(g_offsetRadius)).c_str(),
			timing::TicksToMilliseconds(static_cast<double>(timing::Now() - startTime)));
	}

	// Clears all occlusion maps and the scan state. Called on game load and on exit to the main menu.
	//
	// Thread: Main
	void ResetBuilder() {
		if (g_workerActive) {
			PauseAndRelease();
		}

		ClearMaps();
		ResetScan();

		g_worldSpace = nullptr;
		g_loadedCells.clear();
		g_loadedKeys.clear();
		g_readinessChecks.clear();
		g_statistics = {};
		g_scanRun = {};
		g_framesSinceCellChange = 0;
		g_lastStatusTime = 0;
		g_lastCoveredResolved = 0;
		g_idleLogged = false;
		g_lastPickFaults = engine::GetPickFaultCount();
	}

}
