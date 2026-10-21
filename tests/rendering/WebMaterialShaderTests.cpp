#include <doctest/doctest.h>
#include <vector>
#include "gpu/WebMaterialShader.hpp"

TEST_CASE("Web material shader container rejects ABI and checksum corruption")
{
	std::vector<std::byte> bytes(88 + 4, std::byte{0});
	const auto write = [&](unsigned at, std::uint64_t value, unsigned count)
	{
		for (unsigned index = 0; index < count; ++index)
		{
			bytes[at + index] = std::byte((value >> (index * 8)) & 255);
		}
	};
	bytes[0] = std::byte{'A'};
	bytes[1] = std::byte{'E'};
	bytes[2] = std::byte{'W'};
	bytes[3] = std::byte{'G'};
	write(4, 1, 4);
	write(8, 4, 4);
	for (unsigned index = 0; index < 4; ++index)
	{
		bytes[88 + index] = std::byte{'x'};
	}
	write(16, aether::utils::Fnv1a(bytes.data() + 88, 4), 8);
	CHECK(aether::gpu::DecodeWebMaterialShader(bytes));
	bytes.back() = std::byte{'y'};
	CHECK_FALSE(aether::gpu::DecodeWebMaterialShader(bytes));
	write(4, 2, 4);
	CHECK_FALSE(aether::gpu::DecodeWebMaterialShader(bytes));
}
