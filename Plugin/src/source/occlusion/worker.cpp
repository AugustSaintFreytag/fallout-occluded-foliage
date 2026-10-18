// Async Probe Worker (Experimental)
//
// Main thread preps and queues probe tasks and starts each frame with a time budget.
// The worker executes probes in fixed size bursts. 
// Worker acquires a global Havok lock, runs up to `iBatchSize` probe tasks, then releases lock.
// Game world stepping uses the same lock, so maximum lock-up is how long it takes to get one batch done.
// Bursts follow each other until the frame's time budget is used or the queue is empty.

#include "occlusion/worker.h"

#include "engine/havok.h"
#include "utils/log.h"
#include "utils/timing.h"

#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sof::occlusion {

	namespace {

		// Thread: Any
		std::mutex g_lock;
		std::condition_variable g_wake;  // New frame, new tasks or pause
		std::condition_variable g_burstDone;

		std::deque<ProbeTask> g_queue;
		std::vector<ProbeOutcome> g_outbox;
		WorkerStatistics g_statistics;

		bool g_started = false;
		bool g_running = false;  // Bursts allowed
		bool g_inBurst = false;

		// Thread: Worker
		bool g_havokReady = false;

		std::uint64_t g_frameSerial = 0;
		void* g_cell = nullptr;
		std::int64_t g_allowanceTicks = 0;
		std::uint32_t g_batchSize = 16;
		float g_probeHeight = 512.0f;

		// Probes a given batch of tasks under the global Havok lock. Adds one outcome per task to the given list.
		// Returns the lock wait and the lock hold in counter ticks.
		void RunBurst(void* cell, float probeHeight, const std::vector<ProbeTask>& tasks, std::vector<ProbeOutcome>& outcomes,
			std::int64_t& lockWait, std::int64_t& lockHold) {
			const std::int64_t waitStart = timing::Now();
			const bool locked = engine::LockHavok();
			const std::int64_t holdStart = timing::Now();

			for (const ProbeTask& task : tasks) {
				ProbeOutcome outcome{task.cellX, task.cellY, task.index};

				if (!locked || !cell || !g_havokReady) {
					outcome.faulted = true;
					outcomes.push_back(outcome);
					continue;
				}

				const std::int64_t probeStart = timing::Now();
				const probe::ProbeResult result = probe::ProbePoint(cell, task.x, task.y, task.terrainHeight, probeHeight);

				outcome.verdict = result.verdict;
				outcome.faulted = result.faulted;
				outcome.casts = static_cast<std::uint16_t>(result.casts);
				outcome.microseconds = static_cast<float>(timing::TicksToMicroseconds(static_cast<double>(timing::Now() - probeStart)));
				outcomes.push_back(outcome);
			}

			if (locked) {
				engine::UnlockHavok();
			}

			const std::int64_t holdEnd = timing::Now();

			lockWait = holdStart - waitStart;
			lockHold = holdEnd - holdStart;
		}

		// Waits for frames with tasks and runs bursts until the frame time budget is used.
		//
		// Thread: Worker
		void WorkerLoop() {
			log::SetThreadName("Worker");

			// Havok keeps the name pointer in its thread slot, so the name is a literal
			g_havokReady = engine::InitializeHavokThread("OccludedFoliageWorker");

			if (g_havokReady) {
				SOF_LOG("[Map] Set up Havok thread state for the worker.");
			} else {
				SOF_LOG("[Map] Error: Could not set up Havok thread state. Worker probes are skipped as faults.");
			}

			std::unique_lock lock(g_lock);
			std::uint64_t servedFrame = 0;
			std::vector<ProbeTask> tasks;
			std::vector<ProbeOutcome> outcomes;

			for (;;) {
				g_wake.wait(lock, [&] {
					return g_running && g_frameSerial != servedFrame && !g_queue.empty();
				});

				servedFrame = g_frameSerial;

				void* cell = g_cell;
				const std::int64_t allowance = g_allowanceTicks;
				const std::uint32_t batchSize = g_batchSize;
				const float probeHeight = g_probeHeight;

				std::int64_t used = 0;
				bool ranBurst = false;

				while (g_running && g_frameSerial == servedFrame && !g_queue.empty() && (allowance <= 0 || used < allowance)) {
					tasks.clear();
					outcomes.clear();

					while (!g_queue.empty() && tasks.size() < batchSize) {
						tasks.push_back(g_queue.front());
						g_queue.pop_front();
					}

					g_inBurst = true;
					lock.unlock();

					std::int64_t lockWait = 0;
					std::int64_t lockHold = 0;

					RunBurst(cell, probeHeight, tasks, outcomes, lockWait, lockHold);

					lock.lock();
					g_inBurst = false;
					g_burstDone.notify_all();

					g_outbox.insert(g_outbox.end(), outcomes.begin(), outcomes.end());

					const double lockWaitMilliseconds = timing::TicksToMilliseconds(static_cast<double>(lockWait));
					const double lockHoldMilliseconds = timing::TicksToMilliseconds(static_cast<double>(lockHold));

					++g_statistics.bursts;

					g_statistics.probes += static_cast<std::uint32_t>(outcomes.size());
					g_statistics.busyMilliseconds += lockHoldMilliseconds;
					g_statistics.lockWaitMilliseconds += lockWaitMilliseconds;
					g_statistics.maxLockWaitMilliseconds = std::max(g_statistics.maxLockWaitMilliseconds, lockWaitMilliseconds);
					g_statistics.maxBurstMilliseconds = std::max(g_statistics.maxBurstMilliseconds, lockHoldMilliseconds);

					for (const ProbeOutcome& outcome : outcomes) {
						g_statistics.casts += outcome.casts;
						g_statistics.faults += outcome.faulted ? 1 : 0;
					}

					used += lockHold;
					ranBurst = true;
				}

				if (ranBurst) {
					++g_statistics.frames;
				}
			}
		}

	}

	// Starts the worker thread once for session lifetime.
	//
	// Thread: Main
	void StartWorker() {
		std::lock_guard guard(g_lock);

		if (g_started) {
			return;
		}

		g_started = true;
		std::thread(WorkerLoop).detach();

		SOF_LOG("[Map] Started Havok probe worker thread.");
	}

	// Starts a new frame for the worker.
	// Budget of 0 means to complete the full queue.
	//
	// Thread: Main (each frame)
	void BeginWorkerFrame(void* cell, double budgetMilliseconds, std::uint32_t batchSize, float probeHeight) {
		{
			std::lock_guard guard(g_lock);

			++g_frameSerial;
			g_running = true;
			g_cell = cell;
			g_allowanceTicks = timing::MillisecondsToTicks(budgetMilliseconds);
			g_batchSize = std::max<std::uint32_t>(batchSize, 1);
			g_probeHeight = probeHeight;
		}

		g_wake.notify_one();
	}

	// Stops the worker from starting new bursts and waits for a pending burst to complete.
	// Returns the tasks left over in the task queue.
	//
	// Thread: Main
	std::vector<ProbeTask> PauseWorker() {
		std::unique_lock lock(g_lock);

		g_running = false;
		g_burstDone.wait(lock, [] { return !g_inBurst; });

		std::vector<ProbeTask> tasks(g_queue.begin(), g_queue.end());
		g_queue.clear();

		return tasks;
	}

	// Adds the given tasks to the queue and removes them from the given tasks list.
	//
	// Thread: Main
	void SubmitTasks(std::vector<ProbeTask>& tasks) {
		if (tasks.empty()) {
			return;
		}

		{
			std::lock_guard guard(g_lock);
			g_queue.insert(g_queue.end(), tasks.begin(), tasks.end());
		}

		tasks.clear();
		g_wake.notify_one();
	}

	// Moves all finished outcomes from the outbox to the given list.
	//
	// Thread: Main
	void CollectOutcomes(std::vector<ProbeOutcome>& outcomes) {
		std::lock_guard guard(g_lock);

		outcomes.insert(outcomes.end(), g_outbox.begin(), g_outbox.end());
		g_outbox.clear();
	}

	// Thread: Main
	std::size_t GetQueuedTaskCount() {
		std::lock_guard guard(g_lock);
		return g_queue.size();
	}

	// Returns the worker's statistics and resets them.
	//
	// Thread: Main
	WorkerStatistics TakeWorkerStatistics() {
		std::lock_guard guard(g_lock);

		const WorkerStatistics statistics = g_statistics;
		g_statistics = {};

		return statistics;
	}

}
