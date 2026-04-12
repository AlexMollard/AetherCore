#include "BindlessManager.hpp"

#include <algorithm>
#include <format>

#include "MeowExceptions.hpp"
#include "VulkanContext.hpp"

namespace meow
{
	BindlessManager::~BindlessManager()
	{
		Shutdown();
	}

	void BindlessManager::Initialize(const VulkanContext& context, const Config& config)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device != VK_NULL_HANDLE)
		{
			return;
		}

		if (config.maxSampledImages == 0)
		{
			throw VulkanError("BindlessManager requires at least one sampled-image slot.");
		}

		m_device = context.GetDevice().device;
		m_capacity = config.maxSampledImages;
		m_deferredFreeFrames = config.deferredFreeFrames;
		m_currentFrame = 0;
		m_pendingSlotFrees.clear();

		// ── Immutable linear sampler (binding 1) ──────────────────────────────
		// Created before the layout so the handle can be embedded as immutable.
		const VkSamplerCreateInfo samplerInfo{
			.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter               = VK_FILTER_LINEAR,
			.minFilter               = VK_FILTER_LINEAR,
			.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias              = 0.0f,
			.anisotropyEnable        = VK_FALSE,
			.compareEnable           = VK_FALSE,
			.minLod                  = 0.0f,
			.maxLod                  = VK_LOD_CLAMP_NONE,
			.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
		};
		if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_linearSampler) != VK_SUCCESS)
		{
			throw VulkanError("BindlessManager: failed to create linear sampler.");
		}

		// ── Descriptor pool ───────────────────────────────────────────────────
		const VkDescriptorPoolSize poolSizes[2] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_capacity },
			{ VK_DESCRIPTOR_TYPE_SAMPLER,                1          },
		};

		const VkDescriptorPoolCreateInfo poolCreateInfo{
			.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.pNext         = nullptr,
			.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
			.maxSets       = 1,
			.poolSizeCount = 2,
			.pPoolSizes    = poolSizes,
		};

		const VkResult poolResult = vkCreateDescriptorPool(m_device, &poolCreateInfo, nullptr, &m_pool);
		if (poolResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create bindless descriptor pool. VkResult={}", static_cast<int>(poolResult)));
		}

		// ── Descriptor set layout ─────────────────────────────────────────────
		// binding 0 — COMBINED_IMAGE_SAMPLER array (bindless image array)
		// binding 1 — SAMPLER (immutable linear sampler, shared by all draws)
		const VkDescriptorSetLayoutBinding bindings[2] = {
			{
				.binding         = bindless::kSampledImageBinding, // 0
				.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = m_capacity,
				.stageFlags      = bindless::kDefaultStages,
				.pImmutableSamplers = nullptr,
			},
			{
				.binding            = 1,
				.descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
				.descriptorCount    = 1,
				.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT,
				.pImmutableSamplers = &m_linearSampler, // embedded in the layout
			},
		};

		// VARIABLE_DESCRIPTOR_COUNT must be on the LAST binding; since binding 1
		// (the immutable sampler) is last and has a fixed count of 1, we drop
		// VARIABLE_DESCRIPTOR_COUNT from binding 0 instead.
		const VkDescriptorBindingFlags bindingFlags[2] = {
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
			VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
			0, // immutable sampler needs no special flags
		};

		const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
			.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.pNext        = nullptr,
			.bindingCount = 2,
			.pBindingFlags = bindingFlags,
		};

		const VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
			.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext        = &bindingFlagsInfo,
			.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			.bindingCount = 2,
			.pBindings    = bindings,
		};

		const VkResult layoutResult = vkCreateDescriptorSetLayout(m_device, &layoutCreateInfo, nullptr, &m_layout);
		if (layoutResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create bindless descriptor layout. VkResult={}", static_cast<int>(layoutResult)));
		}

		const VkDescriptorSetAllocateInfo allocateInfo{
			.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.pNext              = nullptr,
			.descriptorPool     = m_pool,
			.descriptorSetCount = 1,
			.pSetLayouts        = &m_layout,
		};

		const VkResult setResult = vkAllocateDescriptorSets(m_device, &allocateInfo, &m_set);
		if (setResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to allocate bindless descriptor set. VkResult={}", static_cast<int>(setResult)));
		}

		m_slotAllocated.assign(m_capacity, false);
		m_freeSlots.reserve(m_capacity);
		for (std::uint32_t slot = 0; slot < m_capacity; ++slot)
		{
			m_freeSlots.push_back(m_capacity - 1 - slot);
		}
	}

	void BindlessManager::Shutdown()
	{
		std::scoped_lock lock(m_mutex);

		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		// Destroy layout before sampler (layout embeds the sampler handle).
		if (m_layout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
			m_layout = VK_NULL_HANDLE;
		}

		if (m_pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(m_device, m_pool, nullptr);
			m_pool = VK_NULL_HANDLE;
		}

		if (m_linearSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(m_device, m_linearSampler, nullptr);
			m_linearSampler = VK_NULL_HANDLE;
		}

		m_set = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
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
		return m_device != VK_NULL_HANDLE;
	}

	VkDescriptorSetLayout BindlessManager::GetLayout() const
	{
		std::scoped_lock lock(m_mutex);
		return m_layout;
	}

	VkDescriptorSet BindlessManager::GetSet() const
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

	std::uint32_t BindlessManager::AllocateSampledImageSlot()
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == VK_NULL_HANDLE)
		{
			throw VulkanError("BindlessManager is not initialized.");
		}

		if (m_freeSlots.empty())
		{
			throw VulkanError("BindlessManager is out of sampled-image slots.");
		}

		const std::uint32_t slot = m_freeSlots.back();
		m_freeSlots.pop_back();
		m_slotAllocated[slot] = true;
		return slot;
	}

	void BindlessManager::FreeSampledImageSlot(const std::uint32_t slot)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		FreeSlotImmediateUnlocked(slot);
	}

	void BindlessManager::FreeSampledImageSlotDeferred(const std::uint32_t slot)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		if (slot >= m_capacity)
		{
			throw VulkanError("BindlessManager slot index out of range.");
		}

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
		if (m_device == VK_NULL_HANDLE || m_pendingSlotFrees.empty())
		{
			return;
		}

		std::erase_if(m_pendingSlotFrees, [&](const PendingSlotFree& pending) {
			if (pending.releaseFrame > m_currentFrame)
			{
				return false;
			}

			FreeSlotImmediateUnlocked(pending.slot);
			return true;
			});
	}

	void BindlessManager::UpdateSampledImage(
		const std::uint32_t slot,
		VkImageView imageView,
		VkSampler sampler,
		const VkImageLayout imageLayout)
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == VK_NULL_HANDLE)
		{
			throw VulkanError("BindlessManager is not initialized.");
		}

		if (slot >= m_capacity)
		{
			throw VulkanError("BindlessManager slot index out of range.");
		}

		if (!m_slotAllocated[slot])
		{
			throw VulkanError("BindlessManager slot must be allocated before update.");
		}

		const VkDescriptorImageInfo imageInfo{
			.sampler = sampler,
			.imageView = imageView,
			.imageLayout = imageLayout,
		};

		const VkWriteDescriptorSet write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = m_set,
			.dstBinding = bindless::kSampledImageBinding,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &imageInfo,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};

		vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
	}

	void BindlessManager::FreeSlotImmediateUnlocked(const std::uint32_t slot)
	{
		if (slot >= m_capacity)
		{
			throw VulkanError("BindlessManager slot index out of range.");
		}

		if (!m_slotAllocated[slot])
		{
			return;
		}

		m_slotAllocated[slot] = false;
		m_freeSlots.push_back(slot);
	}
}
