#pragma once

#include <cstddef>
#include <vector>
#include "utils/Expected.hpp"
#include "vulkan/volk.hpp"

namespace aether::vkutil
{
	Expected<VkShaderModule> CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner);
}
