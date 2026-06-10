#include "gpu/DescriptorSetLayoutOps.hpp"

#include <vector>
#include <vulkan/vulkan.h>

#include "gpu/GpuDevice.hpp" // full type needed for GetVulkanContext()
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether::gpu
{
	[[nodiscard]] Expected<DescriptorSetLayout> CreateDescriptorSetLayout(GpuDevice& device, const DescriptorSetLayoutDesc& desc) noexcept
	{
		// Translate the engine bindings/flags to Vulkan, then call the
		// raw Vulkan entry point. The layout handle is opaque to the
		// engine; release is via DestroyDescriptorSetLayout below.
		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		vkBindings.reserve(desc.bindings.size());
		for (const GpuDescriptorSetLayoutBinding& src: desc.bindings)
		{
			VkDescriptorSetLayoutBinding dst{};
			dst.binding = src.binding;
			dst.descriptorType = ToVk(src.descriptorType);
			dst.descriptorCount = src.descriptorCount;
			dst.stageFlags = ToVk(src.stageFlags);
			dst.pImmutableSamplers = nullptr;
			vkBindings.push_back(dst);
		}

		VkDescriptorSetLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.flags = ToVk(desc.flags);
		info.bindingCount = static_cast<std::uint32_t>(vkBindings.size());
		info.pBindings = vkBindings.data();

		VulkanContext& ctx = device.GetVulkanContext();
		VkDevice vkDevice = ctx.GetDevice().device;
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		if (vkCreateDescriptorSetLayout(vkDevice, &info, nullptr, &layout) != VK_SUCCESS)
		{
			return std::unexpected(AetherError::Vulkan(0, "CreateDescriptorSetLayout: vkCreateDescriptorSetLayout failed."));
		}
		return static_cast<DescriptorSetLayout>(layout);
	}

	void DestroyDescriptorSetLayout(GpuDevice& device, DescriptorSetLayout layout) noexcept
	{
		if (layout == nullptr)
		{
			return;
		}
		VulkanContext& ctx = device.GetVulkanContext();
		vkDestroyDescriptorSetLayout(ctx.GetDevice().device, static_cast<VkDescriptorSetLayout>(layout), nullptr);
	}
} // namespace aether::gpu
