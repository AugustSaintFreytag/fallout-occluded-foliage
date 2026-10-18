#pragma once

#include <cstddef>
#include <cstdint>

namespace sof::mem {

	// Returns a reference to the field at the given byte offset into an engine object.
	template <typename T>
	inline T& Field(void* base, std::uintptr_t offset) {
		return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(base) + offset);
	}

	// Returns the value at a fixed address. Works like an engine singleton pointer.
	template <typename T>
	inline T Global(std::uintptr_t address) {
		return *reinterpret_cast<T*>(address);
	}

	bool SafeRead32(const void* address, std::uint32_t& out);

	bool SafeReadBytes(const void* address, void* out, std::size_t size);

	void* PatchPointer(std::uintptr_t slot, void* replacement);

	bool PatchCall(std::uintptr_t address, std::uintptr_t expectedTarget, void* replacement);

}
