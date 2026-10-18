// Measures where the Havok world step runs, for the async probing assessment.
//
// Hooks `bhkWorld::Update` in both world vtables. Each step records its thread, its duration
// and its start relative to the last main loop message. The main loop measures its own frame time
// and counts the messages that arrive while a step still runs on another thread.
//
// With `[Debug] fPhysicsProbeDelay`, every other 10-second interval busy-waits that long on the stepping thread
// right after each step. If the main thread waits for the physics thread, its frame time rises by the delay.

#include "debug/physics_probe.h"

#include "config/settings.h"
#include "engine/addresses.h"
#include "utils/log.h"
#include "utils/memory.h"
#include "utils/threads.h"
#include "utils/timing.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace sof::debug {

	namespace {

		using UpdateFn = void(__thiscall*)(void* world);

		constexpr std::size_t kMaxThreads = 4;
		constexpr double kStatusIntervalSeconds = 10.0;

		// Steps of one thread since the last status line.
		struct ThreadStatistics {
			std::uint32_t threadId = 0;
			std::uint32_t steps = 0;
			double totalMicroseconds = 0.0;
			double maxMicroseconds = 0.0;
			double totalStartAfterMainLoop = 0.0;  // Microseconds
		};

		UpdateFn g_originalWorldUpdate = nullptr;
		UpdateFn g_originalWorldMUpdate = nullptr;

		// Thread: Any (under the lock)
		std::mutex g_lock;
		ThreadStatistics g_threads[kMaxThreads];
		std::size_t g_threadCount = 0;
		std::uint32_t g_droppedSteps = 0;

		// Thread: Any
		std::atomic<std::uint32_t> g_steppingThread{0};
		std::atomic<std::int64_t> g_lastMainLoop{0};
		std::atomic<std::int64_t> g_delayTicks{0};  // Busy wait after each step, 0 = off

		// Thread: Main
		std::uint32_t g_mainLoops = 0;
		std::uint32_t g_mainLoopsDuringStep = 0;
		double g_totalFrameMilliseconds = 0.0;
		double g_maxFrameMilliseconds = 0.0;
		std::int64_t g_lastStatus = 0;
		bool g_delayInterval = false;

		void RecordStep(std::uint32_t threadId, double microseconds, double startAfterMainLoop) {
			std::lock_guard guard(g_lock);

			ThreadStatistics* statistics = nullptr;

			for (std::size_t index = 0; index < g_threadCount; ++index) {
				if (g_threads[index].threadId == threadId) {
					statistics = &g_threads[index];
				}
			}

			if (!statistics) {
				if (g_threadCount == kMaxThreads) {
					++g_droppedSteps;

					return;
				}

				statistics = &g_threads[g_threadCount++];
				*statistics = {};
				statistics->threadId = threadId;
			}

			++statistics->steps;
			statistics->totalMicroseconds += microseconds;
			statistics->maxMicroseconds = std::max(statistics->maxMicroseconds, microseconds);
			statistics->totalStartAfterMainLoop += startAfterMainLoop;
		}

		// Times one world step and records its thread. Busy-waits after the step in delay intervals.
		//
		// Thread: Any
		void __fastcall Hook_WorldUpdate(void* world, void*) {
			const std::uint32_t threadId = GetCurrentThreadId();
			const std::int64_t start = timing::Now();
			const std::int64_t lastMainLoop = g_lastMainLoop.load(std::memory_order_relaxed);

			g_steppingThread.store(threadId, std::memory_order_relaxed);

			const bool isWorldM = mem::Field<std::uintptr_t>(world, 0) == engine::kVtbl_bhkWorldM;
			(isWorldM ? g_originalWorldMUpdate : g_originalWorldUpdate)(world);

			const std::int64_t end = timing::Now();
			const std::int64_t delayTicks = g_delayTicks.load(std::memory_order_relaxed);

			if (delayTicks > 0) {
				while (timing::Now() - end < delayTicks) {
					YieldProcessor();
				}
			}

			g_steppingThread.store(0, std::memory_order_relaxed);

			const double microseconds = timing::TicksToMicroseconds(static_cast<double>(end - start));
			const double startAfterMainLoop = lastMainLoop ? timing::TicksToMicroseconds(static_cast<double>(start - lastMainLoop)) : 0.0;

			RecordStep(threadId, microseconds, startAfterMainLoop);
		}

		// Returns the number of counter ticks in a given number of milliseconds.
		std::int64_t MillisecondsToTicks(double milliseconds) {
			const double ticksPerMillisecond = 1000.0 / timing::TicksToMicroseconds(1000.0);

			return static_cast<std::int64_t>(milliseconds * ticksPerMillisecond);
		}

		void LogStatus(double delayMilliseconds) {
			std::lock_guard guard(g_lock);

			const double mainLoops = std::max<std::uint32_t>(g_mainLoops, 1);

			SOF_LOG("[Physics] %u main loop messages, %u of them while a step ran on another thread. Frame %.3f ms average, "
					"%.3f ms max. Delay after each step: %.2f ms.",
				g_mainLoops, g_mainLoopsDuringStep, g_totalFrameMilliseconds / mainLoops, g_maxFrameMilliseconds,
				g_delayInterval ? delayMilliseconds : 0.0);

			for (std::size_t index = 0; index < g_threadCount; ++index) {
				const ThreadStatistics& statistics = g_threads[index];
				const double steps = std::max<std::uint32_t>(statistics.steps, 1);

				SOF_LOG("[Physics]   Thread %u (%s): %u steps, %.3f ms average, %.3f ms max, start %.2f ms after the main loop "
						"message on average.",
					statistics.threadId, statistics.threadId == threads::MainThreadId() ? "main" : "other", statistics.steps,
					statistics.totalMicroseconds / steps / 1000.0, statistics.maxMicroseconds / 1000.0,
					statistics.totalStartAfterMainLoop / steps / 1000.0);
			}

			if (g_droppedSteps > 0) {
				SOF_LOG("[Physics]   %u steps on further threads not counted.", g_droppedSteps);
			}

			g_threadCount = 0;
			g_droppedSteps = 0;
			g_mainLoops = 0;
			g_mainLoopsDuringStep = 0;
			g_totalFrameMilliseconds = 0.0;
			g_maxFrameMilliseconds = 0.0;
		}

	}

	// Patches `bhkWorld::Update` in the `bhkWorld` and `bhkWorldM` vtables if `[Debug] bProbePhysics` is on.
	// Returns false if a patch failed.
	//
	// Thread: Main (load time only)
	bool InstallPhysicsProbe() {
		if (!config::Get().debug.probePhysics) {
			return true;
		}

		g_originalWorldUpdate = static_cast<UpdateFn>(mem::PatchPointer(engine::kVtbl_bhkWorld + engine::kWorldVtbl_Update,
			reinterpret_cast<void*>(&Hook_WorldUpdate)));
		g_originalWorldMUpdate = static_cast<UpdateFn>(mem::PatchPointer(engine::kVtbl_bhkWorldM + engine::kWorldVtbl_Update,
			reinterpret_cast<void*>(&Hook_WorldUpdate)));

		SOF_LOG("Hooks: bhkWorld::Update %p, bhkWorldM::Update %p -> %p", g_originalWorldUpdate, g_originalWorldMUpdate,
			reinterpret_cast<void*>(&Hook_WorldUpdate));

		return g_originalWorldUpdate && g_originalWorldMUpdate;
	}

	// Measures the frame time, notes whether a step runs on another thread right now,
	// and logs the status every 10 seconds. Switches the delay on and off between intervals.
	//
	// Thread: Main (each frame)
	void UpdatePhysicsProbe() {
		if (!g_originalWorldUpdate) {
			return;
		}

		const std::int64_t now = timing::Now();
		const std::int64_t lastMainLoop = g_lastMainLoop.load(std::memory_order_relaxed);
		const std::uint32_t steppingThread = g_steppingThread.load(std::memory_order_relaxed);

		++g_mainLoops;

		if (steppingThread != 0 && steppingThread != threads::MainThreadId()) {
			++g_mainLoopsDuringStep;
		}

		if (lastMainLoop != 0) {
			const double frameMilliseconds = timing::TicksToMilliseconds(static_cast<double>(now - lastMainLoop));
			g_totalFrameMilliseconds += frameMilliseconds;
			g_maxFrameMilliseconds = std::max(g_maxFrameMilliseconds, frameMilliseconds);
		}

		g_lastMainLoop.store(now, std::memory_order_relaxed);

		if (g_lastStatus == 0) {
			g_lastStatus = now;

			return;
		}

		if (timing::TicksToMicroseconds(static_cast<double>(now - g_lastStatus)) < kStatusIntervalSeconds * 1.0e6) {
			return;
		}

		const double delayMilliseconds = config::Get().debug.physicsProbeDelay;

		LogStatus(delayMilliseconds);

		g_delayInterval = delayMilliseconds > 0.0 && !g_delayInterval;
		g_delayTicks.store(g_delayInterval ? MillisecondsToTicks(delayMilliseconds) : 0, std::memory_order_relaxed);
		g_lastStatus = now;
	}

}
