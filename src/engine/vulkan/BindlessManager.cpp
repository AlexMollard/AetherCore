#include "gpu/BindlessManager.hpp"

#include <algorithm>
#include <format>

#include "utils/Assert.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	BindlessManager::~BindlessManager()
	{
		Shutdown();
	}

	Expected<void> BindlessManager::Initialize(const VulkanContext& context, const Config& config)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device != nullptr)
		{
			return {};
		}

		AE_ASSERT(config.maxSampledImages > 0, "BindlessManager requires at least one sampled-image slot.");

		m_device = static_cast<gpu::Device>(context.GetDevice().device);
		m_capacity = config.maxSampledImages;
		m_deferredFreeFrames = config.deferredFreeFrames;
		m_currentFrame = 0;
		m_pendingSlotFrees.clear();

		// -- Immutable linear sampler (binding 1) ------------------------------
		// Created before the layout so the handle can be embedded as immutable.
		const VkSamplerCreateInfo samplerInfo{
		        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		        .magFilter = VK_FILTER_LINEAR,
		        .minFilter = VK_FILTER_LINEAR,
		        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		        .compareEnable = VK_FALSE,
		        .minLod = 0.0f,
		        .maxLod = VK_LOD_CLAMP_NONE,
		        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
		        .unnormalizedCoordinates = VK_FALSE,
		};
		VkSampler linearSampler = VK_NULL_HANDLE;
		if (vkCreateSampler(static_cast<VkDevice>(m_device), &samplerInfo, nullptr, &linearSampler) != VK_SUCCESS)
		{
			m_device = nullptr;
			return Unexpected{AetherError::Vulkan(0, "BindlessManager: failed to create linear sampler.")};
		}
		m_linearSampler = static_cast<gpu::Sampler>(linearSampler);

		// -- Descriptor pool ---------------------------------------------------
		const VkDescriptorPoolSize poolSizes[2] = {
		        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_capacity},
		        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
		};

		const VkDescriptorPoolCreateInfo poolCreateInfo{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
		        .maxSets = 1,
		        .poolSizeCount = 2,
		        .pPoolSizes = poolSizes,
		};

		VkDescriptorPool pool = VK_NULL_HANDLE;
		const VkResult poolResult = vkCreateDescriptorPool(static_cast<VkDevice>(m_device), &poolCreateInfo, nullptr, &pool);
		if (poolResult != VK_SUCCESS)
		{
			vkDestroySampler(static_cast<VkDevice>(m_device), linearSampler, nullptr);
			m_linearSampler = nullptr;
			m_device = nullptr;
			return Unexpected{AetherError::Vulkan(static_cast<int32_t>(poolResult), "Failed to create bindless descriptor pool.")};
		}
		m_pool = static_cast<gpu::DescriptorPool>(pool);

		// -- Descriptor set layout ---------------------------------------------
		// binding 0 - COMBINED_IMAGE_SAMPLER array (bindless image array)
		// binding 1 - SAMPLER (immutable linear sampler, shared by most draws)
		const VkDescriptorSetLayoutBinding bindings[2] = {
		        {
		                .binding = bindless::kSampledImageBinding, // 0
		                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                .descriptorCount = m_capacity,
		                .stageFlags = VK_SHADER_STAGE_ALL,
		                .pImmutableSamplers = nullptr,
		        },
		        {
		                .binding = 1,
		                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
		                .descriptorCount = 1,
		                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		                .pImmutableSamplers = &linearSampler, // embedded in the layout
		        },
		};

		// VARIABLE_DESCRIPTOR_COUNT must be on the LAST binding; since binding 1
		// (immutable linear sampler) is last and has a fixed count of 1, we drop
		// VARIABLE_DESCRIPTOR_COUNT from binding 0 instead.
		const VkDescriptorBindingFlags bindingFlags[2] = {
		        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
		        0, // immutable sampler needs no special flags
		};

		const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		        .pNext = nullptr,
		        .bindingCount = 2,
		        .pBindingFlags = bindingFlags,
		};

		const VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		        .pNext = &bindingFlagsInfo,
		        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		        .bindingCount = 2,
		        .pBindings = bindings,
		};

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		const VkResult layoutResult = vkCreateDescriptorSetLayout(static_cast<VkDevice>(m_device), &layoutCreateInfo, nullptr, &layout);
		if (layoutResult != VK_SUCCESS)
		{
			vkDestroyDescriptorPool(static_cast<VkDevice>(m_device), pool, nullptr);
			vkDestroySampler(static_cast<VkDevice>(m_device), linearSampler, nullptr);
			m_pool = nullptr;
			m_linearSampler = nullptr;
			m_device = nullptr;
			return Unexpected{AetherError::Vulkan(static_cast<int32_t>(layoutResult), "Failed to create bindless descriptor layout.")};
		}
		m_layout = static_cast<gpu::DescriptorSetLayout>(layout);

		const VkDescriptorSetAllocateInfo allocateInfo{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		        .pNext = nullptr,
		        .descriptorPool = pool,
		        .descriptorSetCount = 1,
		        .pSetLayouts = &layout,
		};

		VkDescriptorSet set = VK_NULL_HANDLE;
		const VkResult setResult = vkAllocateDescriptorSets(static_cast<VkDevice>(m_device), &allocateInfo, &set);
		if (setResult != VK_SUCCESS)
		{
			vkDestroyDescriptorSetLayout(static_cast<VkDevice>(m_device), layout, nullptr);
			vkDestroyDescriptorPool(static_cast<VkDevice>(m_device), pool, nullptr);
			vkDestroySampler(static_cast<VkDevice>(m_device), linearSampler, nullptr);
			m_layout = nullptr;
			m_pool = nullptr;
			m_linearSampler = nullptr;
			m_device = nullptr;
			return Unexpected{AetherError::Vulkan(static_cast<int32_t>(setResult), "Failed to allocate bindless descriptor set.")};
		}
		m_set = static_cast<gpu::DescriptorSet>(set);

		m_slotAllocated.assign(m_capacity, false);
		m_freeSlots.reserve(m_capacity);
		for (std::uint32_t slot = 0; slot < m_capacity; ++slot)
		{
			m_freeSlots.push_back(m_capacity - 1 - slot);
		}

		return {};
	}

	void BindlessManager::Shutdown()
	{
		std::scoped_lock lock(m_mutex);

		if (m_device == nullptr)
		{
			return;
		}

		const VkDevice vkDevice = static_cast<VkDevice>(m_device);

		// Destroy layout before sampler (layout embeds the sampler handle).
		if (m_layout != nullptr)
		{
			vkDestroyDescriptorSetLayout(vkDevice, static_cast<VkDescriptorSetLayout>(m_layout), nullptr);
			m_layout = nullptr;
		}

		if (m_pool != nullptr)
		{
			vkDestroyDescriptorPool(vkDevice, static_cast<VkDescriptorPool>(m_pool), nullptr);
			m_pool = nullptr;
		}

		if (m_linearSampler != nullptr)
		{
			vkDestroySampler(vkDevice, static_cast<VkSampler>(m_linearSampler), nullptr);
			m_linearSampler = nullptr;
		}

		for (const auto& [key, sampler]: m_samplerCache)
		{
			vkDestroySampler(vkDevice, static_cast<VkSampler>(sampler), nullptr);
		}
		m_samplerCache.clear();

		m_set = nullptr;
		m_device = nullptr;
		m_capacity = 0;
		m_deferredFreeFrames = 3;
		m_currentFrame = 0;
		m_freeSlots.clear();
		m_slotAllocated.clear();
		m_pendingSlotFrees.clear();
	}

	bool BindlessManager::IsInitialized() const
	{
		std::scoped_lock lock(m_mutex);
		return m_device != nullptr;
	}

	gpu::DescriptorSetLayout BindlessManager::GetLayout() const
	{
		std::scoped_lock lock(m_mutex);
		return m_layout;
	}

	gpu::DescriptorSet BindlessManager::GetSet() const
	{
		std::scoped_lock lock(m_mutex);
		return m_set;
	}

	std::uint32_t BindlessManager::GetCapacity() const
	{
		std::scoped_lock lock(m_mutex);
		return m_capacity;
	}

	std::uint64_t BindlessManager::GetCurrentFrame() const
	{
		std::scoped_lock lock(m_mutex);
		return m_currentFrame;
	}

	Expected<std::uint32_t> BindlessManager::AllocateSampledImageSlot()
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return Unexpected{AetherError::Engine("BindlessManager is not initialized.")};
		}

		if (m_freeSlots.empty())
		{
			return Unexpected{AetherError::Engine("BindlessManager is out of sampled-image slots.")};
		}

		const std::uint32_t slot = m_freeSlots.back();
		m_freeSlots.pop_back();
		m_slotAllocated[slot] = true;
		return slot;
	}

	Expected<gpu::Sampler> BindlessManager::GetOrCreateSampler(const gpu::Filter filter, const gpu::SamplerMipmapMode mipmapMode, const gpu::SamplerAddressMode addressMode)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return Unexpected{AetherError::Engine("BindlessManager is not initialized.")};
		}

		const SamplerKey key{filter, mipmapMode, addressMode};
		const auto it = m_samplerCache.find(key);
		if (it != m_samplerCache.end())
		{
			return it->second;
		}

		const VkFilter vkFilter = gpu::ToVk(filter);
		const VkSamplerMipmapMode vkMipmap = gpu::ToVk(mipmapMode);
		const VkSamplerAddressMode vkAddress = gpu::ToVk(addressMode);

		const VkSamplerCreateInfo samplerInfo{
		        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		        .magFilter = vkFilter,
		        .minFilter = vkFilter,
		        .mipmapMode = vkMipmap,
		        .addressModeU = vkAddress,
		        .addressModeV = vkAddress,
		        .addressModeW = vkAddress,
		        .compareEnable = VK_FALSE,
		        .minLod = 0.0f,
		        .maxLod = VK_LOD_CLAMP_NONE,
		        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
		        .unnormalizedCoordinates = VK_FALSE,
		};

		VkSampler sampler = VK_NULL_HANDLE;
		if (vkCreateSampler(static_cast<VkDevice>(m_device), &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(0, "BindlessManager: failed to create cached sampler.")};
		}

		const gpu::Sampler gpuSampler = static_cast<gpu::Sampler>(sampler);
		m_samplerCache[key] = gpuSampler;
		return gpuSampler;
	}

	void BindlessManager::FreeSampledImageSlot(const std::uint32_t slot)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return;
		}

		FreeSlotImmediateUnlocked(slot);
	}

	void BindlessManager::FreeSampledImageSlotDeferred(const std::uint32_t slot)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return;
		}

		AE_ASSERT_ALWAYS(slot < m_capacity, "BindlessManager slot index out of range.");

		if (!m_slotAllocated[slot])
		{
			return;
		}

		const auto releaseFrame = m_currentFrame + static_cast<std::uint64_t>(m_deferredFreeFrames);
		m_pendingSlotFrees.push_back(PendingSlotFree{
		        .slot = slot,
		        .releaseFrame = releaseFrame,
		});
	}

	void BindlessManager::AdvanceFrame(const std::uint64_t frameIndex)
	{
		std::scoped_lock lock(m_mutex);
		m_currentFrame = frameIndex;
		if (m_device == nullptr || m_pendingSlotFrees.empty())
		{
			return;
		}

		std::erase_if(m_pendingSlotFrees,
		        [&](const PendingSlotFree& pending)
		        {
			        if (pending.releaseFrame > m_currentFrame)
			        {
				        return false;
			        }

			        FreeSlotImmediateUnlocked(pending.slot);
			        return true;
		        });
	}

	Expected<void> BindlessManager::UpdateSampledImage(const std::uint32_t slot, const gpu::ImageView imageView, const gpu::Sampler sampler, const gpu::ImageLayout imageLayout)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return Unexpected{AetherError::Engine("BindlessManager is not initialized.")};
		}

		if (slot >= m_capacity)
		{
			return Unexpected{AetherError::Engine("BindlessManager slot index out of range.")};
		}

		if (!m_slotAllocated[slot])
		{
			return Unexpected{AetherError::Engine("BindlessManager slot must be allocated before update.")};
		}

		const VkDescriptorImageInfo imageInfo{
		        .sampler = static_cast<VkSampler>(sampler),
		        .imageView = static_cast<VkImageView>(imageView),
		        .imageLayout = gpu::ToVk(imageLayout),
		};

		const VkWriteDescriptorSet write{
		        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		        .pNext = nullptr,
		        .dstSet = static_cast<VkDescriptorSet>(m_set),
		        .dstBinding = bindless::kSampledImageBinding,
		        .dstArrayElement = slot,
		        .descriptorCount = 1,
		        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		        .pImageInfo = &imageInfo,
		        .pBufferInfo = nullptr,
		        .pTexelBufferView = nullptr,
		};

		vkUpdateDescriptorSets(static_cast<VkDevice>(m_device), 1, &write, 0, nullptr);
		return {};
	}

	void BindlessManager::FreeSlotImmediateUnlocked(const std::uint32_t slot)
	{
		AE_ASSERT_ALWAYS(slot < m_capacity, "BindlessManager slot index out of range.");

		if (!m_slotAllocated[slot])
		{
			return;
		}

		m_slotAllocated[slot] = false;
		m_freeSlots.push_back(slot);
	}
} // namespace aether
