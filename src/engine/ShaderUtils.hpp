#pragma once

#include <cstddef>
#include <vector>
#include <vulkan/vulkan.h>

namespace aether::vkutil
{
	VkShaderModule CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner);
}
