#pragma once
#include "rendering/GraphicsPipeline.hpp"
#include <vector>

// The fake driver never opens shader files. Supply its matching CPU capability
// fixture explicitly so cache tests stay independent of whichever VFS is mounted.
inline aether::Expected<std::vector<std::byte>> ReadFakeLegacyShader(std::string_view)
{
	constexpr std::uint32_t words[] = {
	        0x07230203,
	        0x00010000,
	        0,
	        5,
	        0,
	        0x00020011,
	        1,
	        0x0003000e,
	        0,
	        1,
	        0x0007000f,
	        4,
	        1,
	        0x67617266,
	        0x746e656d,
	        0x6e69614d,
	        0,
	        0x00030010,
	        1,
	        7,
	        0x00020013,
	        2,
	        0x00030021,
	        3,
	        2,
	        0x00050036,
	        2,
	        1,
	        0,
	        3,
	        0x000200f8,
	        4,
	        0x000100fd,
	        0x00010038,
	};
	std::vector<std::byte> bytes;
	for (auto word: words)
	{
		for (unsigned i = 0; i < 4; ++i)
		{
			bytes.push_back(std::byte((word >> (i * 8)) & 255));
		}
	}
	return bytes;
}

struct FakePipelineFactory
{
	int buildCount = 0;

	aether::Expected<aether::GraphicsPipeline> operator()(const aether::GraphicsPipeline::Desc&)
	{
		++buildCount;
		return aether::GraphicsPipeline{};
	}
};
