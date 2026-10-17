#pragma once
// Engine addresses and object layouts for FalloutNV.exe 1.4.0.525.
// Each value has a tag that shows its source, as in `Documentation/Engine Reference.md`:
//   EXE   = verified in the disassembly of the unpacked exe
//   SEA   = Saint's Experimental Acoustics, verified in game there
//   TSTG  = There Stands the Grass
//   JIP, JG, xNVSE = JIP LN NVSE, JohnnyGuitar NVSE, xNVSE

#include <cstdint>

namespace sof::engine {

	// Functions
	constexpr std::uintptr_t kQueueUIMessage = 0x7052F0;  // Corner notification (JIP, xNVSE, SEA)

}
