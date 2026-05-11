#pragma once

#include <cstddef>
#include <vector>
#include "vulkan/volk.hpp"

namespace aether::vkutil
{
	VkShaderModule CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner);
}
