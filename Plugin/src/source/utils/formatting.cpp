#include "utils/formatting.h"

#include <algorithm>
#include <cstdio>

namespace sof::format {

	// Formats a count with thousands separators, as in "16,384".
	//
	// Thread: Any
	std::string Count(std::uint64_t value) {
		std::string digits = std::to_string(value);

		for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(digits.size()) - 3; position > 0; position -= 3) {
			digits.insert(static_cast<std::size_t>(position), ",");
		}

		return digits;
	}

	// Returns "s" for counts other than 1.
	//
	// Thread: Any
	const char* Plural(std::uint64_t count) {
		return count == 1 ? "" : "s";
	}

	// Formats a duration in seconds as "45 s" or "3 min 20 s".
	//
	// Thread: Any
	std::string Duration(double seconds) {
		const std::uint64_t total = static_cast<std::uint64_t>(std::max(seconds, 0.0) + 0.5);
		char text[32];

		if (total < 60) {
			std::snprintf(text, sizeof(text), "%llus", total);
		} else {
			std::snprintf(text, sizeof(text), "%llum %llus", total / 60, total % 60);
		}

		return text;
	}

}
