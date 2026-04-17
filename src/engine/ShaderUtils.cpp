#include "ShaderUtils.hpp"

#include <stdexcept>
#include <string>

namespace aether::vkutil
{
	VkShaderModule CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner)
	{
		const VkShaderModuleCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = spirv.size(),
			.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data()),
		};

		VkShaderModule mod = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
		{
			throw std::runtime_error(std::string(owner) + ": failed to create shader module.");
		}

		return mod;
	}
} // namespace aether::vkutil
