#include "gpu/GpuDeviceFactory.hpp"

#include "vulkan/volk.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanUtils.hpp"

#include "utils/Assert.hpp"

namespace aether::gpu::Factory
{
	// -----------------------------------------------------------------
	// CommandPool
	// -----------------------------------------------------------------

	CommandPool CreateCommandPool(Device device, const CommandPoolDesc& desc) noexcept
	{
		AE_ASSERT(device != nullptr, "CreateCommandPool: device is null");

		VkCommandPoolCreateFlags flags = 0;
		if (desc.transient)
		{
			flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		}
		if (desc.resetCommandBuffer)
		{
			flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		}

		const VkCommandPoolCreateInfo createInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = flags,
		        .queueFamilyIndex = desc.queueFamilyIndex,
		};

		VkCommandPool vkPool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(static_cast<VkDevice>(device), &createInfo, nullptr, &vkPool) != VK_SUCCESS)
		{
			return nullptr;
		}
		return static_cast<CommandPool>(vkPool);
	}

	void DestroyCommandPool(Device device, CommandPool pool) noexcept
	{
		if (pool == nullptr)
		{
			return;
		}
		vkDestroyCommandPool(static_cast<VkDevice>(device), static_cast<VkCommandPool>(pool), nullptr);
	}

	// -----------------------------------------------------------------
	// QueryPool
	// -----------------------------------------------------------------

	QueryPool CreateQueryPool(Device device, const QueryPoolDesc& desc) noexcept
	{
		AE_ASSERT(device != nullptr, "CreateQueryPool: device is null");

		VkQueryType vkType = VK_QUERY_TYPE_TIMESTAMP;
		switch (desc.type)
		{
			case QueryType::Timestamp:
				vkType = VK_QUERY_TYPE_TIMESTAMP;
				break;
			case QueryType::Occlusion:
				vkType = VK_QUERY_TYPE_OCCLUSION;
				break;
			case QueryType::PipelineStatistics:
				vkType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
				break;
		}

		const VkQueryPoolCreateInfo createInfo{
		        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		        .queryType = vkType,
		        .queryCount = desc.count,
		};

		VkQueryPool vkPool = VK_NULL_HANDLE;
		if (vkCreateQueryPool(static_cast<VkDevice>(device), &createInfo, nullptr, &vkPool) != VK_SUCCESS)
		{
			return nullptr;
		}
		return static_cast<QueryPool>(vkPool);
	}

	void DestroyQueryPool(Device device, QueryPool pool) noexcept
	{
		if (pool == nullptr)
		{
			return;
		}
		vkDestroyQueryPool(static_cast<VkDevice>(device), static_cast<VkQueryPool>(pool), nullptr);
	}

	void ResetQueryPool(Device device, QueryPool pool, std::uint32_t firstQuery, std::uint32_t queryCount) noexcept
	{
		if (pool == nullptr)
		{
			return;
		}
		vkResetQueryPool(static_cast<VkDevice>(device), static_cast<VkQueryPool>(pool), firstQuery, queryCount);
	}

	// -----------------------------------------------------------------
	// ShaderModule
	// -----------------------------------------------------------------

	Pipeline CreateShaderModule(Device device, const SpirvBlob& spirv, const char* debugName) noexcept
	{
		AE_ASSERT(device != nullptr, "CreateShaderModule: device is null");
		AE_ASSERT(spirv.data != nullptr && spirv.size > 0, "CreateShaderModule: empty SPIR-V blob");

		const VkShaderModuleCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		        .codeSize = spirv.size,
		        .pCode = reinterpret_cast<const std::uint32_t*>(spirv.data),
		};

		VkShaderModule mod = VK_NULL_HANDLE;
		if (vkCreateShaderModule(static_cast<VkDevice>(device), &info, nullptr, &mod) != VK_SUCCESS)
		{
			return nullptr;
		}
		if (debugName != nullptr)
		{
			vkutil::SetObjectName(static_cast<VkDevice>(device), reinterpret_cast<std::uint64_t>(mod), VK_OBJECT_TYPE_SHADER_MODULE, debugName);
		}
		return static_cast<Pipeline>(mod);
	}

	void DestroyShaderModule(Device device, Pipeline shader) noexcept
	{
		if (shader == nullptr)
		{
			return;
		}
		vkDestroyShaderModule(static_cast<VkDevice>(device), static_cast<VkShaderModule>(shader), nullptr);
	}

	// -----------------------------------------------------------------
	// DescriptorSetLayout
	// -----------------------------------------------------------------

	DescriptorSetLayout CreateDescriptorSetLayout(Device device, const DescriptorSetLayoutDesc& desc) noexcept
	{
		AE_ASSERT(device != nullptr, "CreateDescriptorSetLayout: device is null");

		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		vkBindings.reserve(desc.bindings.size());
		for (const auto& b: desc.bindings)
		{
			vkBindings.push_back(VkDescriptorSetLayoutBinding{
			        .binding = b.binding,
			        .descriptorType = gpu::ToVk(b.descriptorType),
			        .descriptorCount = b.descriptorCount,
			        .stageFlags = gpu::ToVk(b.stageFlags),
			        .pImmutableSamplers = nullptr,
			});
		}

		const VkDescriptorSetLayoutCreateFlags flags = desc.pushDescriptor ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT : 0u;
		const VkDescriptorSetLayoutCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		        .flags = flags,
		        .bindingCount = static_cast<std::uint32_t>(vkBindings.size()),
		        .pBindings = vkBindings.data(),
		};

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		if (vkCreateDescriptorSetLayout(static_cast<VkDevice>(device), &info, nullptr, &layout) != VK_SUCCESS)
		{
			return nullptr;
		}
		return static_cast<DescriptorSetLayout>(layout);
	}

	void DestroyDescriptorSetLayout(Device device, DescriptorSetLayout layout) noexcept
	{
		if (layout == nullptr)
		{
			return;
		}
		vkDestroyDescriptorSetLayout(static_cast<VkDevice>(device), static_cast<VkDescriptorSetLayout>(layout), nullptr);
	}

	// -----------------------------------------------------------------
	// PipelineLayout
	// -----------------------------------------------------------------

	PipelineLayout CreatePipelineLayout(Device device, const PipelineLayoutDesc& desc) noexcept
	{
		AE_ASSERT(device != nullptr, "CreatePipelineLayout: device is null");

		std::vector<VkPushConstantRange> vkRanges;
		vkRanges.reserve(desc.pushConstantRanges.size());
		for (const auto& r: desc.pushConstantRanges)
		{
			vkRanges.push_back(VkPushConstantRange{
			        .stageFlags = gpu::ToVk(r.stageFlags),
			        .offset = r.offset,
			        .size = r.size,
			});
		}

		const VkPipelineLayoutCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = static_cast<std::uint32_t>(desc.setLayouts.size()),
		        .pSetLayouts = reinterpret_cast<const VkDescriptorSetLayout*>(desc.setLayouts.data()),
		        .pushConstantRangeCount = static_cast<std::uint32_t>(vkRanges.size()),
		        .pPushConstantRanges = vkRanges.data(),
		};

		VkPipelineLayout layout = VK_NULL_HANDLE;
		if (vkCreatePipelineLayout(static_cast<VkDevice>(device), &info, nullptr, &layout) != VK_SUCCESS)
		{
			return nullptr;
		}
		return static_cast<PipelineLayout>(layout);
	}

	void DestroyPipelineLayout(Device device, PipelineLayout layout) noexcept
	{
		if (layout == nullptr)
		{
			return;
		}
		vkDestroyPipelineLayout(static_cast<VkDevice>(device), static_cast<VkPipelineLayout>(layout), nullptr);
	}

	// -----------------------------------------------------------------
	// Fence
	// -----------------------------------------------------------------

	Fence CreateFence(Device device, const FenceDesc& desc) noexcept
	{
		AE_ASSERT(device != nullptr, "CreateFence: device is null");

		const VkFenceCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		        .flags = desc.signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0u,
		};

		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(static_cast<VkDevice>(device), &info, nullptr, &fence) != VK_SUCCESS)
		{
			return nullptr;
		}
		return static_cast<Fence>(fence);
	}

	void DestroyFence(Device device, Fence fence) noexcept
	{
		if (fence == nullptr)
		{
			return;
		}
		vkDestroyFence(static_cast<VkDevice>(device), static_cast<VkFence>(fence), nullptr);
	}

	bool WaitFence(Device device, Fence fence, std::uint64_t timeoutNs) noexcept
	{
		if (fence == nullptr)
		{
			return false;
		}
		const VkResult res = vkWaitForFences(static_cast<VkDevice>(device), 1, reinterpret_cast<VkFence*>(&fence), VK_TRUE, timeoutNs);
		return res == VK_SUCCESS;
	}

	void ResetFence(Device device, Fence fence) noexcept
	{
		if (fence == nullptr)
		{
			return;
		}
		vkResetFences(static_cast<VkDevice>(device), 1, reinterpret_cast<VkFence*>(&fence));
	}

	// -----------------------------------------------------------------
	// PhysicalDevice queries
	// -----------------------------------------------------------------

	PhysicalDeviceProperties GetPhysicalDeviceProperties(PhysicalDevice physicalDevice) noexcept
	{
		AE_ASSERT(physicalDevice != nullptr, "GetPhysicalDeviceProperties: physicalDevice is null");
		PhysicalDeviceProperties out{};
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(physicalDevice), &props);
		out.limits.timestampComputeAndGraphics = (props.limits.timestampComputeAndGraphics == VK_TRUE);
		out.limits.timestampPeriod = props.limits.timestampPeriod;
		return out;
	}

	std::uint32_t GetQueryPoolResults(Device device, QueryPool pool, std::uint32_t firstQuery, std::uint32_t queryCount, std::span<std::uint64_t> outTicks) noexcept
	{
		if (pool == nullptr || queryCount == 0)
		{
			return 0;
		}
		const std::uint32_t readable = std::min(queryCount, static_cast<std::uint32_t>(outTicks.size()));
		if (readable == 0)
		{
			return 0;
		}
		const VkResult result = vkGetQueryPoolResults(static_cast<VkDevice>(device), static_cast<VkQueryPool>(pool), firstQuery, readable, readable * sizeof(std::uint64_t), outTicks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
		return result == VK_SUCCESS ? readable : 0;
	}

} // namespace aether::gpu::Factory
