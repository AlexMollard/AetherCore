#include "memory/MemoryBackend.hpp"

#include <cstddef>
#include <cstdio>
#include <string_view>

#include "Defines.hpp"

int main(int argc, char** argv)
{
	aether_memory_force_link();
#if !AE_ADDRESS_SANITIZER
	std::fputs("ASAN instrumentation absent\n", stderr);
	return 2;
#else
	if (aether::memory::IsMimallocActive())
	{
		std::fputs("Engine allocator replacement bypasses ASAN\n", stderr);
		return 3;
	}
	std::fputs("ASAN engine allocation probe ready\n", stderr);
	std::fflush(stderr);
	auto* bytes = new volatile unsigned char[8];
	const bool valid = argc == 2 && std::string_view(argv[1]) == "--valid";
	volatile std::size_t index = valid ? 7 : 8;
	bytes[index] = 42; // Intentional heap-buffer-overflow in the isolated child only.
	delete[] bytes;
	return 0;
#endif
}
