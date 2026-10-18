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

	// Replaces a pointer in read-only memory, such as a vtable slot.
	// Returns the previous value, or null if the replacement failed.
	//
	// Thread: Main (load time only)
	void* PatchPointer(std::uintptr_t slot, void* replacement) {
		auto* target = reinterpret_cast<void**>(slot);
		DWORD oldProtection = 0;

		if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
			return nullptr;
		}

		void* previous = *target;
		*target = replacement;
		VirtualProtect(target, sizeof(void*), oldProtection, &oldProtection);

		return previous;
	}

	// Redirects a `call rel32` at a given address to a replacement function.
	// Patches only if the call goes to the expected target. Returns false otherwise or if the write failed.
	//
	// Thread: Main (load time only)
	bool PatchCall(std::uintptr_t address, std::uintptr_t expectedTarget, void* replacement) {
		auto* code = reinterpret_cast<std::uint8_t*>(address);

		if (code[0] != 0xE8) {
			return false;
		}

		const std::uintptr_t target = address + 5 + *reinterpret_cast<std::int32_t*>(code + 1);

		if (target != expectedTarget) {
			return false;
		}

		DWORD oldProtection = 0;

		if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
			return false;
		}

		*reinterpret_cast<std::int32_t*>(code + 1) =
			static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(replacement) - (address + 5));

		VirtualProtect(code, 5, oldProtection, &oldProtection);
		FlushInstructionCache(GetCurrentProcess(), code, 5);

		return true;
	}

}
