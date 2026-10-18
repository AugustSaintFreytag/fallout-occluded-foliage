#include "utils/log.h"

#include "utils/threads.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sof::log {

	namespace {

		std::mutex g_lock;
		std::FILE* g_file = nullptr;
		ULONGLONG g_startTime = 0;

		thread_local const char* g_threadName = nullptr;

	}

	void Open(const char* path) {
		std::lock_guard guard(g_lock);

		if (g_file) {
			return;
		}

		g_file = _fsopen(path, "w", _SH_DENYWR);
		g_startTime = GetTickCount64();
	}

	// Writes one line with the seconds since the log opened and the name of the calling thread.
	// Threads without a name show as "Main" or by their ID.
	//
	// Thread: Any
	void Write(const char* format, ...) {
		char line[1024];

		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(line, sizeof(line), format, arguments);
		va_end(arguments);

		char thread[16];

		if (g_threadName) {
			std::snprintf(thread, sizeof(thread), "%s", g_threadName);
		} else if (threads::IsMainThread()) {
			std::snprintf(thread, sizeof(thread), "Main");
		} else {
			std::snprintf(thread, sizeof(thread), "%lu", GetCurrentThreadId());
		}

		std::lock_guard guard(g_lock);

		if (!g_file) {
			return;
		}

		const ULONGLONG elapsed = GetTickCount64() - g_startTime;

		std::fprintf(g_file, "%6llu.%03llu  %-7s %s\n", elapsed / 1000, elapsed % 1000, thread, line);
		std::fflush(g_file);
	}

	// Sets the name that log lines from the calling thread show. The name must outlive the thread.
	//
	// Thread: Any
	void SetThreadName(const char* name) {
		g_threadName = name;
	}

}
