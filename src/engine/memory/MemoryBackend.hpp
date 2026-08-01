#pragma once

namespace aether::memory
{
	// Whether this process's `operator new` is actually the mimalloc replacement.
	//
	// Linking mimalloc is not the same as USING it. A replacement `operator new` defined in a
	// static library is only linked in if something pulls its object file in, so a build can
	// link mimalloc successfully and still route every allocation through the CRT. This
	// answers the real question by allocating and asking mimalloc whether it owns the block,
	// rather than by checking that the library is present.
	[[nodiscard]] bool IsMimallocActive() noexcept;
} // namespace aether::memory

// Anchors the override translation unit so the linker cannot discard it.
//
// The overrides are the only thing in that TU, and nothing calls them by name - they are
// selected by the compiler at each `new` site - so without a referenced symbol the object
// file is never pulled out of Engine.lib and the replacement silently does not happen.
// Executables reference this from startup.
extern "C" void aether_memory_force_link() noexcept;
