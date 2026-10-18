#pragma once

#include <cstdint>
#include <string>

namespace sof::format {

	std::string Count(std::uint64_t value);

	const char* Plural(std::uint64_t count);

	std::string Duration(double seconds);

}
