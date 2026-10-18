#pragma once

#include <Windows.h>

#include <cstdint>

namespace sof::timing {

	// Returns the current `QueryPerformanceCounter` ticks.
	inline std::int64_t Now() {
		LARGE_INTEGER counter{};
		QueryPerformanceCounter(&counter);

		return counter.QuadPart;
	}

	// Returns the `QueryPerformanceCounter` ticks per second.
	inline std::int64_t Frequency() {
		static const std::int64_t frequency = [] {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);

			return value.QuadPart;
		}();

		return frequency;
	}

	// Converts counter ticks to microseconds.
	inline double TicksToMicroseconds(double ticks) {
		return ticks * 1'000'000.0 / static_cast<double>(Frequency());
	}

	// Converts counter ticks to milliseconds.
	inline double TicksToMilliseconds(double ticks) {
		return TicksToMicroseconds(ticks) / 1000.0;
	}

	// Converts milliseconds to counter ticks.
	inline std::int64_t MillisecondsToTicks(double milliseconds) {
		return static_cast<std::int64_t>(milliseconds * static_cast<double>(Frequency()) / 1000.0);
	}

}
