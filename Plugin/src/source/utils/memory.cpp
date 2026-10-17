#include "utils/memory.h"

#include <Windows.h>

namespace sof::mem {

	// Reads 4 bytes from the given address into the given value.
	// Returns false if the address cannot be read.
	//
	// Thread: Any
	bool SafeRead32(const void* address, std::uint32_t& out) {
		__try {
			out = *static_cast<const volatile std::uint32_t*>(address);

			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

}
