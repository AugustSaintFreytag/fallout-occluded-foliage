#pragma once

#include "probe/probe.h"

#include <cstdint>
#include <vector>

namespace sof::occlusion {

	// Library

	// A grid point for the async worker to probe.
	struct ProbeTask {
		std::int32_t cellX = 0;
		std::int32_t cellY = 0;
		std::int32_t index = 0;  // Grid point index in the cell map
		float x = 0.0f;  // Game units
		float y = 0.0f;
		float terrainHeight = 0.0f;
	};

	// The result of a probe task.
	struct ProbeOutcome {
		std::int32_t cellX = 0;
		std::int32_t cellY = 0;
		std::int32_t index = 0;
		probe::Verdict verdict = probe::Verdict::Clear;
		bool faulted = false;  // No valid verdict. The grid point needs another probe.
		std::uint16_t casts = 0;
		float microseconds = 0.0f;
	};

	// Counters of the async worker since the last time they were taken.
	struct WorkerStatistics {
		std::uint32_t frames = 0;  // Frames in which the worker ran at least one burst
		std::uint32_t bursts = 0;
		std::uint32_t probes = 0;
		std::uint32_t casts = 0;
		std::uint32_t faults = 0;
		double busyMilliseconds = 0.0;  // Probing time, lock waits excluded
		double lockWaitMilliseconds = 0.0;
		double maxLockWaitMilliseconds = 0.0;
		double maxBurstMilliseconds = 0.0;  // Longest lock hold
	};

	// Lifecycle

	void StartWorker();
	void BeginWorkerFrame(void* cell, double budgetMilliseconds, std::uint32_t batchSize, float probeHeight);
	std::vector<ProbeTask> PauseWorker();

	void SubmitTasks(std::vector<ProbeTask>& tasks);
	void CollectOutcomes(std::vector<ProbeOutcome>& outcomes);

	std::size_t GetQueuedTaskCount();
	WorkerStatistics TakeWorkerStatistics();

}
