#include "vulkan/ShaderUtils.hpp"

#include <format>

#include "rendering/CommandRecorder.hpp"

namespace aether::vkutil
{
	Expected<VkShaderModule> CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner)
	{
		const VkShaderModuleCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		        .codeSize = spirv.size(),
		        .pCode = reinterpret_cast<const std::uint32_t*>(spirv.data()),
		};

		VkShaderModule mod = VK_NULL_HANDLE;
		const VkResult result = vkCreateShaderModule(device, &info, nullptr, &mod);
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), std::string(owner) + ": failed to create shader module."));
		}

		CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(mod), VK_OBJECT_TYPE_SHADER_MODULE, owner);

		return mod;
	}
} // namespace aether::vkutil
