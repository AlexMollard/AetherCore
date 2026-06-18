#include "vulkan/ShaderUtils.hpp"

#include <format>

#include "vulkan/VulkanUtils.hpp"

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
#	include "vulkan/AftermathContext.hpp"
#endif

namespace aether::vkutil
{
	Expected<UniqueShaderModule> CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv, const char* owner)
	{
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		AftermathContext::RegisterShaderBinary(spirv.data(), static_cast<uint32_t>(spirv.size()));
#endif

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

		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(mod), VK_OBJECT_TYPE_SHADER_MODULE, owner);

		return UniqueShaderModule{device, mod};
	}
} // namespace aether::vkutil
