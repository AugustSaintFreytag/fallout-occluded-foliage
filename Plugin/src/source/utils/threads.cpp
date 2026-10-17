#include "utils/threads.h"

#include <Windows.h>

#include <atomic>

namespace sof::threads {

	namespace {

		std::atomic<std::uint32_t> g_mainThreadId{0};

	}

	// Stores the calling thread as the main thread.
	// Called from `NVSEPlugin_Load` and on each main loop message.
	//
	// Thread: Main
	void RememberMainThread() {
		g_mainThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	}

	// Thread: Any
	bool IsMainThread() {
		return GetCurrentThreadId() == g_mainThreadId.load(std::memory_order_relaxed);
	}

	// Thread: Any
	std::uint32_t MainThreadId() {
		return g_mainThreadId.load(std::memory_order_relaxed);
	}

}
