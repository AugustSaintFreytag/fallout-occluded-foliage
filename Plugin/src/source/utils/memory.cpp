#include "utils/memory.h"

#include <Windows.h>

#include <cstring>

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

	// Copies the given number of bytes from the given address into the given buffer.
	// Returns false if any byte cannot be read. The buffer may then hold a partial copy.
	//
	// Thread: Any
	bool SafeReadBytes(const void* address, void* out, std::size_t size) {
		__try {
			std::memcpy(out, address, size);

			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

}
