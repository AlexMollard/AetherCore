#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "memory/MemoryBackend.hpp"

// The allocator overrides live alone in their own translation unit and are selected by
// the compiler at each `new` site, so nothing ever references them by name and the
// linker drops the object file out of Engine.lib. The engine's own entry point anchors
// them for the same reason; this executable has to do it too, or the memory tracking
// tests measure a process whose `operator new` was never replaced - which is exactly
// what they exist to catch.
int main(int argc, char** argv)
{
	aether_memory_force_link();
	return doctest::Context(argc, argv).run();
}
