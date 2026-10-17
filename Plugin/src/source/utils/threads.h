#pragma once

#include <cstdint>

namespace sof::threads {

	void RememberMainThread();

	bool IsMainThread();

	std::uint32_t MainThreadId();

}
