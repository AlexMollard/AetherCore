#include "gpu/BindlessManager.hpp"

#include <algorithm>
#include <format>
#include <numeric>

#include "utils/Assert.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	namespace
	{
		VkDeviceSize AlignUp(const VkDeviceSize value, const VkDeviceSize alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		struct DescriptorHeapMappings
		{
			VkDescriptorSetAndBindingMappingEXT mappings[2];
			VkShaderDescriptorSetAndBindingMappingInfoEXT shaderMappingInfo;
		};
	} // namespace

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
		m_vmaAllocator = reinterpret_cast<void*>(context.GetAllocator());
		m_capacity = config.maxSampledImages;
		m_deferredFreeFrames = config.deferredFreeFrames;
		m_currentFrame = 0;
		m_pendingSlotFrees.clear();

		m_slotAllocated.assign(m_capacity, false);
		m_freeSlots.reserve(m_capacity);
		for (std::uint32_t slot = 0; slot < m_capacity; ++slot)
		{
			m_freeSlots.push_back(m_capacity - 1 - slot);
		}

		// ── Descriptor heaps (VK_EXT_descriptor_heap) ──────────────────────────
		const auto& heapProps = context.GetDescriptorHeapProperties();
		m_imageDescriptorSize = heapProps.imageDescriptorSize;
		m_imageDescriptorAlignment = heapProps.imageDescriptorAlignment;
		m_imageDescriptorStride = AlignUp(m_imageDescriptorSize, m_imageDescriptorAlignment);
		m_samplerDescriptorSize = heapProps.samplerDescriptorSize;
		m_samplerDescriptorAlignment = heapProps.samplerDescriptorAlignment;
		m_resourceHeapReservedRangeSize = heapProps.minResourceHeapReservedRange;
		m_samplerHeapReservedRangeSize = heapProps.minSamplerHeapReservedRange;

		AE_INFO(LogCategory::Vulkan,
		        "BindlessManager: imageDescriptorSize={}, imageDescriptorAlignment={}, samplerDescriptorSize={}, samplerDescriptorAlignment={}",
		        m_imageDescriptorSize,
		        m_imageDescriptorAlignment,
		        m_samplerDescriptorSize,
		        m_samplerDescriptorAlignment);

		const VmaAllocator allocator = context.GetAllocator();
		const VkBufferUsageFlags2 heapUsage = VK_BUFFER_USAGE_2_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;

		// Resource heap (SAMPLED_IMAGE descriptors)
		const VkDeviceSize resourceDescriptorBytes = static_cast<VkDeviceSize>(m_capacity) * m_imageDescriptorStride;
		const VkDeviceSize resourceReservedAlignment = std::lcm(heapProps.bufferDescriptorAlignment, heapProps.imageDescriptorAlignment);
		m_resourceHeapReservedRangeOffset = AlignUp(resourceDescriptorBytes, resourceReservedAlignment);
		m_resourceHeapSize = m_resourceHeapReservedRangeOffset + m_resourceHeapReservedRangeSize;

		{
			const VkBufferCreateInfo bufferInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			        .pNext = nullptr,
			        .size = m_resourceHeapSize,
			        .usage = 0,
			};

			const VkBufferUsageFlags2CreateInfo usageInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
			        .pNext = nullptr,
			        .usage = heapUsage,
			};

			VkBufferCreateInfo mutableInfo = bufferInfo;
			mutableInfo.pNext = &usageInfo;

			const VmaAllocationCreateInfo allocInfo{
			        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			        .usage = VMA_MEMORY_USAGE_AUTO,
			};

			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocationInfo allocDetail{};
			const VkResult result = vmaCreateBuffer(allocator, &mutableInfo, &allocInfo, &buffer, &allocation, &allocDetail);
			if (result != VK_SUCCESS)
			{
				ShutdownUnlocked();
				return Unexpected{AetherError::Vulkan(static_cast<int32_t>(result), "BindlessManager: failed to create resource descriptor heap.")};
			}

			m_resourceHeapBuffer = static_cast<void*>(buffer);
			m_resourceHeapAlloc = static_cast<void*>(allocation);
			m_resourceHeapMapped = allocDetail.pMappedData;
			AE_ASSERT(m_resourceHeapMapped != nullptr, "VMA_MAPPED_BIT should yield persistent mapped pointer");

			// Query device address
			const VkBufferDeviceAddressInfo addrInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
			m_resourceHeapAddr = vkGetBufferDeviceAddress(static_cast<VkDevice>(m_device), &addrInfo);
			if (m_memoryTracker != nullptr && m_resourceHeapAddr != 0)
			{
				m_memoryTracker->Register(m_resourceHeapAddr, m_resourceHeapSize, "bindless_resource_heap", GpuMemoryTracker::ResourceType::BindlessHeap);
			}
		}

		// Sampler heap (one immutable linear SAMPLER)
		m_samplerHeapReservedRangeOffset = AlignUp(m_samplerDescriptorSize, m_samplerDescriptorAlignment);
		m_samplerHeapSize = m_samplerHeapReservedRangeOffset + m_samplerHeapReservedRangeSize;

		{
			const VkBufferCreateInfo bufferInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			        .pNext = nullptr,
			        .size = m_samplerHeapSize,
			        .usage = 0,
			};

			const VkBufferUsageFlags2CreateInfo usageInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
			        .pNext = nullptr,
			        .usage = heapUsage,
			};

			VkBufferCreateInfo mutableInfo = bufferInfo;
			mutableInfo.pNext = &usageInfo;

			const VmaAllocationCreateInfo allocInfo{
			        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			        .usage = VMA_MEMORY_USAGE_AUTO,
			};

			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocationInfo allocDetail{};
			const VkResult result = vmaCreateBuffer(allocator, &mutableInfo, &allocInfo, &buffer, &allocation, &allocDetail);
			if (result != VK_SUCCESS)
			{
				ShutdownUnlocked();
				return Unexpected{AetherError::Vulkan(static_cast<int32_t>(result), "BindlessManager: failed to create sampler descriptor heap.")};
			}

			m_samplerHeapBuffer = static_cast<void*>(buffer);
			m_samplerHeapAlloc = static_cast<void*>(allocation);
			m_samplerHeapMapped = allocDetail.pMappedData;
			AE_ASSERT(m_samplerHeapMapped != nullptr, "VMA_MAPPED_BIT should yield persistent mapped pointer");

			const VkBufferDeviceAddressInfo addrInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
			m_samplerHeapAddr = vkGetBufferDeviceAddress(static_cast<VkDevice>(m_device), &addrInfo);
			if (m_memoryTracker != nullptr && m_samplerHeapAddr != 0)
			{
				m_memoryTracker->Register(m_samplerHeapAddr, m_samplerHeapSize, "bindless_sampler_heap", GpuMemoryTracker::ResourceType::BindlessHeap);
			}
		}

		AE_INFO(LogCategory::Vulkan, "BindlessManager: resource heap {}B at 0x{:016x}, sampler heap {}B at 0x{:016x}", m_resourceHeapSize, m_resourceHeapAddr, m_samplerHeapSize, m_samplerHeapAddr);

		WriteLinearSamplerUnlocked();

		// ── Pipeline mapping info ─────────────────────────────────────────────
		{
			auto* pm = new DescriptorHeapMappings;

			// Set 0 binding 0: g_textures[] → resource heap (SAMPLED_IMAGE array).
			pm->mappings[0] = {
			        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
			        .pNext = nullptr,
			        .descriptorSet = 0,
			        .firstBinding = 0,
			        .bindingCount = 1,
			        .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT,
			        .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
			};
			pm->mappings[0].sourceData.constantOffset = {
			        .heapOffset = 0,
			        .heapArrayStride = static_cast<std::uint32_t>(m_imageDescriptorStride),
			        .pEmbeddedSampler = nullptr,
			        .samplerHeapOffset = 0,
			        .samplerHeapArrayStride = 0,
			};

			// Set 0 binding 1: g_linearSampler → sampler heap (SAMPLER).
			pm->mappings[1] = {
			        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
			        .pNext = nullptr,
			        .descriptorSet = 0,
			        .firstBinding = 1,
			        .bindingCount = 1,
			        .resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
			        .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
			};
			pm->mappings[1].sourceData.constantOffset = {
			        .heapOffset = 0,
			        .heapArrayStride = 0,
			        .pEmbeddedSampler = nullptr,
			        .samplerHeapOffset = 0,
			        .samplerHeapArrayStride = 0,
			};

			pm->shaderMappingInfo = {
			        .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
			        .pNext = nullptr,
			        .mappingCount = 2,
			        .pMappings = pm->mappings,
			};

			m_shaderMappingInfo = &pm->shaderMappingInfo;
		}

		return {};
	}

	void BindlessManager::Shutdown()
	{
		std::scoped_lock lock(m_mutex);
		ShutdownUnlocked();
	}

	void BindlessManager::ShutdownUnlocked()
	{
		if (m_device == nullptr)
		{
			return;
		}

		// Descriptor heaps
		if (m_resourceHeapBuffer != nullptr)
		{
			if (m_memoryTracker != nullptr && m_resourceHeapAddr != 0)
			{
				m_memoryTracker->Unregister(m_resourceHeapAddr);
			}
			vmaDestroyBuffer(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VkBuffer>(m_resourceHeapBuffer), static_cast<VmaAllocation>(m_resourceHeapAlloc));
			m_resourceHeapBuffer = nullptr;
			m_resourceHeapAlloc = nullptr;
			m_resourceHeapMapped = nullptr;
			m_resourceHeapAddr = 0;
			m_resourceHeapSize = 0;
			m_resourceHeapReservedRangeOffset = 0;
			m_resourceHeapReservedRangeSize = 0;
		}
		if (m_samplerHeapBuffer != nullptr)
		{
			if (m_memoryTracker != nullptr && m_samplerHeapAddr != 0)
			{
				m_memoryTracker->Unregister(m_samplerHeapAddr);
			}
			vmaDestroyBuffer(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VkBuffer>(m_samplerHeapBuffer), static_cast<VmaAllocation>(m_samplerHeapAlloc));
			m_samplerHeapBuffer = nullptr;
			m_samplerHeapAlloc = nullptr;
			m_samplerHeapMapped = nullptr;
			m_samplerHeapAddr = 0;
			m_samplerHeapSize = 0;
			m_samplerHeapReservedRangeOffset = 0;
			m_samplerHeapReservedRangeSize = 0;
		}
		m_imageDescriptorSize = 0;
		m_imageDescriptorAlignment = 0;
		m_imageDescriptorStride = 0;
		m_samplerDescriptorSize = 0;
		m_samplerDescriptorAlignment = 0;

		if (m_shaderMappingInfo != nullptr)
		{
			// The DescriptorHeapMappings struct (containing mappings + shaderMappingInfo)
			// was allocated as a single block; the pointer points to the embedded
			// VkShaderDescriptorSetAndBindingMappingInfoEXT inside it.
			auto* pm = reinterpret_cast<DescriptorHeapMappings*>(reinterpret_cast<std::byte*>(m_shaderMappingInfo) - offsetof(DescriptorHeapMappings, shaderMappingInfo));
			delete pm;
			m_shaderMappingInfo = nullptr;
		}
		m_device = nullptr;
		m_vmaAllocator = nullptr;
		m_capacity = 0;
		m_deferredFreeFrames = 3;
		m_currentFrame = 0;
		m_freeSlots.clear();
		m_slotAllocated.clear();
		m_pendingSlotFrees.clear();
	}

	std::uint32_t BindlessManager::GetCapacity() const
	{
		std::scoped_lock lock(m_mutex);
		return m_capacity;
	}

	Expected<gpu::Sampler> BindlessManager::CreateSampler(const gpu::Filter filter, const gpu::SamplerMipmapMode mipmap, const gpu::SamplerAddressMode address) const
	{
		std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return Unexpected{AetherError::Engine("BindlessManager is not initialized.")};
		}

		const VkSamplerCreateInfo samplerInfo{
		        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		        .magFilter = gpu::ToVk(filter),
		        .minFilter = gpu::ToVk(filter),
		        .mipmapMode = gpu::ToVk(mipmap),
		        .addressModeU = gpu::ToVk(address),
		        .addressModeV = gpu::ToVk(address),
		        .addressModeW = gpu::ToVk(address),
		        .compareEnable = VK_FALSE,
		        .minLod = 0.0f,
		        .maxLod = VK_LOD_CLAMP_NONE,
		        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
		        .unnormalizedCoordinates = VK_FALSE,
		};

		VkSampler sampler = VK_NULL_HANDLE;
		if (vkCreateSampler(static_cast<VkDevice>(m_device), &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(0, "BindlessManager: failed to create sampler.")};
		}

		return static_cast<gpu::Sampler>(sampler);
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

	// ── Descriptor-heap API ───────────────────────────────────────────────────

	gpu::DeviceAddress BindlessManager::GetResourceHeapAddress() const
	{
		std::scoped_lock lock(m_mutex);
		return m_resourceHeapAddr;
	}

	gpu::DeviceAddress BindlessManager::GetSamplerHeapAddress() const
	{
		std::scoped_lock lock(m_mutex);
		return m_samplerHeapAddr;
	}

	gpu::DeviceSize BindlessManager::GetResourceHeapSize() const
	{
		std::scoped_lock lock(m_mutex);
		return m_resourceHeapSize;
	}

	gpu::DeviceSize BindlessManager::GetSamplerHeapSize() const
	{
		std::scoped_lock lock(m_mutex);
		return m_samplerHeapSize;
	}

	gpu::DeviceSize BindlessManager::GetImageDescriptorSize() const
	{
		std::scoped_lock lock(m_mutex);
		return m_imageDescriptorSize;
	}

	const void* BindlessManager::GetDescriptorHeapMappings() const
	{
		return m_shaderMappingInfo;
	}

	Expected<void> BindlessManager::WriteSampledImage(const std::uint32_t slot, const void* viewCreateInfo, const gpu::ImageLayout layout)
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
			return Unexpected{AetherError::Engine("BindlessManager slot must be allocated before write.")};
		}

		const auto* pViewInfo = static_cast<const VkImageViewCreateInfo*>(viewCreateInfo);

		const VkImageDescriptorInfoEXT imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
		        .pNext = nullptr,
		        .pView = pViewInfo,
		        .layout = gpu::ToVk(layout),
		};

		const VkResourceDescriptorInfoEXT resourceInfo{
		        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		        .pNext = nullptr,
		        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		        .data = {.pImage = &imageInfo},
		};

		const VkDeviceSize offset = static_cast<VkDeviceSize>(slot) * m_imageDescriptorStride;
		void* hostAddr = static_cast<std::byte*>(m_resourceHeapMapped) + offset;

		const VkHostAddressRangeEXT hostRange{
		        .address = hostAddr,
		        .size = m_imageDescriptorSize,
		};

		vkWriteResourceDescriptorsEXT(static_cast<VkDevice>(m_device), 1, &resourceInfo, &hostRange);
		const VkResult flushResult = vmaFlushAllocation(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VmaAllocation>(m_resourceHeapAlloc), offset, m_imageDescriptorSize);
		if (flushResult != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(static_cast<std::int32_t>(flushResult), "BindlessManager: failed to flush a sampled-image descriptor write.")};
		}

		return {};
	}

	void BindlessManager::WriteLinearSampler()
	{
		std::scoped_lock lock(m_mutex);
		WriteLinearSamplerUnlocked();
	}

	void BindlessManager::WriteLinearSamplerUnlocked()
	{
		if (m_device == nullptr || m_samplerHeapMapped == nullptr)
		{
			return;
		}

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

		const VkHostAddressRangeEXT hostRange{
		        .address = m_samplerHeapMapped,
		        .size = m_samplerDescriptorSize,
		};

		vkWriteSamplerDescriptorsEXT(static_cast<VkDevice>(m_device), 1, &samplerInfo, &hostRange);
		const VkResult flushResult = vmaFlushAllocation(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VmaAllocation>(m_samplerHeapAlloc), 0, m_samplerDescriptorSize);
		AE_ASSERT_ALWAYS(flushResult == VK_SUCCESS, "BindlessManager: failed to flush the sampler descriptor write.");
	}

	void BindlessManager::CmdBindHeaps(gpu::CommandList& cmd) const
	{
		if (m_resourceHeapBuffer == nullptr || m_samplerHeapBuffer == nullptr)
		{
			return;
		}

		const auto vkCmd = static_cast<VkCommandBuffer>(cmd.GetCommandBuffer());

		const VkBindHeapInfoEXT resourceBindInfo{
		        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
		        .heapRange =
		                {
		                        .address = m_resourceHeapAddr,
		                        .size = m_resourceHeapSize,
		                },
		        .reservedRangeOffset = m_resourceHeapReservedRangeOffset,
		        .reservedRangeSize = m_resourceHeapReservedRangeSize,
		};
		vkCmdBindResourceHeapEXT(vkCmd, &resourceBindInfo);

		const VkBindHeapInfoEXT samplerBindInfo{
		        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
		        .heapRange =
		                {
		                        .address = m_samplerHeapAddr,
		                        .size = m_samplerHeapSize,
		                },
		        .reservedRangeOffset = m_samplerHeapReservedRangeOffset,
		        .reservedRangeSize = m_samplerHeapReservedRangeSize,
		};
		vkCmdBindSamplerHeapEXT(vkCmd, &samplerBindInfo);
	}

	void BindlessManager::SetMemoryTracker(GpuMemoryTracker* tracker)
	{
		m_memoryTracker = tracker;
		if (m_memoryTracker != nullptr)
		{
			if (m_resourceHeapAddr != 0)
			{
				m_memoryTracker->Register(m_resourceHeapAddr, m_resourceHeapSize, "bindless_resource_heap", GpuMemoryTracker::ResourceType::BindlessHeap);
			}
			if (m_samplerHeapAddr != 0)
			{
				m_memoryTracker->Register(m_samplerHeapAddr, m_samplerHeapSize, "bindless_sampler_heap", GpuMemoryTracker::ResourceType::BindlessHeap);
			}
		}
	}
} // namespace aether
