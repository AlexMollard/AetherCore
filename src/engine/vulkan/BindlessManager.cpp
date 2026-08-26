#include "gpu/BindlessManager.hpp"

#include <algorithm>
#include <format>
#include <numeric>

#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "vulkan/GlobalBindingLayout.hpp"
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

	namespace vulkan
	{
		namespace
		{
			GlobalBindingLayout g_globalBindingLayout{};
		} // namespace

		void SetGlobalBindingLayout(const GlobalBindingLayout& layout)
		{
			g_globalBindingLayout = layout;
		}

		const GlobalBindingLayout& GetGlobalBindingLayout()
		{
			return g_globalBindingLayout;
		}
	} // namespace vulkan

	BindlessManager::~BindlessManager()
	{
		Shutdown();
	}

	Expected<void> BindlessManager::Initialize(const VulkanContext& context, const Config& config)
	{
		AE_PROFILE_ZONE();
		const std::scoped_lock lock(m_mutex);
		if (m_device != nullptr)
		{
			return {};
		}

		AE_ASSERT(config.maxSampledImages > 0, "BindlessManager requires at least one sampled-image slot.");

		m_device = static_cast<gpu::Device>(context.GetDevice().device);
		m_vmaAllocator = reinterpret_cast<void*>(context.GetAllocator());

		{
			// Cache the anisotropy ceiling once. It is a device limit, and every sampler
			// below has to clamp to it or sampler creation is invalid.
			VkPhysicalDeviceProperties deviceProps{};
			vkGetPhysicalDeviceProperties(context.GetDevice().physical_device, &deviceProps);
			m_maxAnisotropy = std::min(deviceProps.limits.maxSamplerAnisotropy, static_cast<float>(std::max(config.maxAnisotropy, 1u)));
		}
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

		m_useDescriptorHeap = context.SupportsDescriptorHeap();
		if (!m_useDescriptorHeap)
		{
			Expected<void> fallback = InitializeDescriptorBufferBackendUnlocked(context);
			if (!fallback)
			{
				ShutdownUnlocked();
				return fallback;
			}
			return {};
		}

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

			const VkBufferDeviceAddressInfo addrInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
			m_resourceHeapAddr = vkGetBufferDeviceAddress(static_cast<VkDevice>(m_device), &addrInfo);
			if (m_memoryTracker != nullptr && m_resourceHeapAddr != 0)
			{
				m_memoryTracker->Register(m_resourceHeapAddr, m_resourceHeapSize, "bindless_resource_heap", GpuMemoryTracker::ResourceType::BindlessHeap);
			}
		}

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

		{
			auto* pm = new DescriptorHeapMappings;

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

	Expected<void> BindlessManager::InitializeDescriptorBufferBackendUnlocked(const VulkanContext& context)
	{
		AE_PROFILE_ZONE();
		auto* const device = static_cast<VkDevice>(m_device);

		VkPhysicalDeviceDescriptorBufferPropertiesEXT bufferProps{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props2{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		        .pNext = &bufferProps,
		};
		vkGetPhysicalDeviceProperties2(context.GetDevice().physical_device, &props2);

		// Every push block in the engine is asserted against kMaxGuaranteedPushConstantSize at
		// compile time, so a device at or above the spec floor fits all of them. A device BELOW
		// the floor is out of spec, but say so precisely here rather than letting
		// vkCreateShadersEXT fail later with a bare error code.
		if (props2.properties.limits.maxPushConstantsSize < gpu::kMaxGuaranteedPushConstantSize)
		{
			return Unexpected{AetherError::Vulkan(0,
			        std::format("BindlessManager: device reports maxPushConstantsSize={}, below the {}-byte minimum the Vulkan spec guarantees; the renderer's push blocks cannot be bound.",
			                props2.properties.limits.maxPushConstantsSize,
			                gpu::kMaxGuaranteedPushConstantSize))};
		}
		const auto pushConstantSize = static_cast<std::uint32_t>(gpu::kMaxGuaranteedPushConstantSize);

		m_imageDescriptorSize = bufferProps.sampledImageDescriptorSize;
		m_samplerDescriptorSize = bufferProps.samplerDescriptorSize;

		const VkDescriptorSetLayoutBinding bindings[2] = {
		        {
		                .binding = 0,
		                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		                .descriptorCount = m_capacity,
		                .stageFlags = VK_SHADER_STAGE_ALL,
		                .pImmutableSamplers = nullptr,
		        },
		        {
		                .binding = 1,
		                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
		                .descriptorCount = 1,
		                .stageFlags = VK_SHADER_STAGE_ALL,
		                .pImmutableSamplers = nullptr,
		        },
		};

		// DESCRIPTOR_BUFFER_BIT is what makes this layout addressable by offset instead of
		// allocatable from a pool. There is no pool and no descriptor set on this path.
		const VkDescriptorSetLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
		        .bindingCount = 2,
		        .pBindings = bindings,
		};

		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		if (const VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout); result != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(static_cast<std::int32_t>(result), "BindlessManager: failed to create the global descriptor set layout.")};
		}
		m_setLayout = static_cast<void*>(setLayout);

		// The driver decides the layout's byte size and where each binding sits inside it -
		// never compute these by hand, the packing is implementation-defined.
		VkDeviceSize layoutSize = 0;
		vkGetDescriptorSetLayoutSizeEXT(device, setLayout, &layoutSize);
		vkGetDescriptorSetLayoutBindingOffsetEXT(device, setLayout, 0, &m_imageBindingOffset);
		vkGetDescriptorSetLayoutBindingOffsetEXT(device, setLayout, 1, &m_samplerBindingOffset);
		m_resourceHeapSize = AlignUp(layoutSize, bufferProps.descriptorBufferOffsetAlignment);

		const VkBufferUsageFlags2 descriptorUsage = VK_BUFFER_USAGE_2_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;

		const VkBufferUsageFlags2CreateInfo usageInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .pNext = nullptr,
		        .usage = descriptorUsage,
		};

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageInfo,
		        .size = m_resourceHeapSize,
		        .usage = 0,
		};

		const VmaAllocationCreateInfo allocInfo{
		        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		        .usage = VMA_MEMORY_USAGE_AUTO,
		};

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo allocDetail{};
		const auto allocator = reinterpret_cast<VmaAllocator>(m_vmaAllocator);
		if (const VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer, &allocation, &allocDetail); result != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(static_cast<std::int32_t>(result), "BindlessManager: failed to create the descriptor buffer.")};
		}

		m_resourceHeapBuffer = static_cast<void*>(buffer);
		m_resourceHeapAlloc = static_cast<void*>(allocation);
		m_resourceHeapMapped = allocDetail.pMappedData;
		AE_ASSERT(m_resourceHeapMapped != nullptr, "VMA_MAPPED_BIT should yield persistent mapped pointer");

		const VkBufferDeviceAddressInfo addrInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = buffer};
		m_resourceHeapAddr = vkGetBufferDeviceAddress(device, &addrInfo);
		if (m_memoryTracker != nullptr && m_resourceHeapAddr != 0)
		{
			m_memoryTracker->Register(m_resourceHeapAddr, m_resourceHeapSize, "bindless_descriptor_buffer", GpuMemoryTracker::ResourceType::BindlessHeap);
		}

		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_ALL,
		        .offset = 0,
		        .size = pushConstantSize,
		};

		const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = 0,
		        .setLayoutCount = 1,
		        .pSetLayouts = &setLayout,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &pushRange,
		};

		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		if (const VkResult result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout); result != VK_SUCCESS)
		{
			return Unexpected{AetherError::Vulkan(static_cast<std::int32_t>(result), "BindlessManager: failed to create the global pipeline layout.")};
		}
		m_pipelineLayout = static_cast<void*>(pipelineLayout);

		vulkan::SetGlobalBindingLayout(vulkan::GlobalBindingLayout{
		        .setLayout = setLayout,
		        .pipelineLayout = pipelineLayout,
		        .pushConstantSize = pushConstantSize,
		});

		WriteLinearSamplerUnlocked();

		AE_INFO(LogCategory::Vulkan,
		        "BindlessManager: descriptor-buffer backend ready - {}B buffer at 0x{:016x}, {} slots, sampledImageDescriptorSize={}, samplerDescriptorSize={}, imageBindingOffset={}, samplerBindingOffset={}.",
		        m_resourceHeapSize,
		        m_resourceHeapAddr,
		        m_capacity,
		        m_imageDescriptorSize,
		        m_samplerDescriptorSize,
		        m_imageBindingOffset,
		        m_samplerBindingOffset);
		return {};
	}

	void BindlessManager::ShutdownDescriptorBufferBackendUnlocked()
	{
		auto* const device = static_cast<VkDevice>(m_device);

		if (m_pipelineLayout != nullptr)
		{
			vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_pipelineLayout), nullptr);
			m_pipelineLayout = nullptr;
		}
		if (m_setLayout != nullptr)
		{
			vkDestroyDescriptorSetLayout(device, static_cast<VkDescriptorSetLayout>(m_setLayout), nullptr);
			m_setLayout = nullptr;
		}
		if (m_fallbackSampler != nullptr)
		{
			vkDestroySampler(device, static_cast<VkSampler>(m_fallbackSampler), nullptr);
			m_fallbackSampler = nullptr;
		}
		m_imageBindingOffset = 0;
		m_samplerBindingOffset = 0;
		vulkan::SetGlobalBindingLayout(vulkan::GlobalBindingLayout{});
	}

	void BindlessManager::Shutdown()
	{
		AE_PROFILE_ZONE();
		const std::scoped_lock lock(m_mutex);
		ShutdownUnlocked();
	}

	void BindlessManager::ShutdownUnlocked()
	{
		if (m_device == nullptr)
		{
			return;
		}

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

		if (!m_useDescriptorHeap)
		{
			ShutdownDescriptorBufferBackendUnlocked();
		}

		if (m_shaderMappingInfo != nullptr)
		{
			auto* pm = reinterpret_cast<DescriptorHeapMappings*>(reinterpret_cast<std::byte*>(m_shaderMappingInfo) - offsetof(DescriptorHeapMappings, shaderMappingInfo));
			delete pm;
			m_shaderMappingInfo = nullptr;
		}
		m_device = nullptr;
		m_vmaAllocator = nullptr;
		m_useDescriptorHeap = true;
		m_capacity = 0;
		m_deferredFreeFrames = 3;
		m_currentFrame = 0;
		m_freeSlots.clear();
		m_slotAllocated.clear();
		m_pendingSlotFrees.clear();
	}

	std::uint32_t BindlessManager::GetCapacity() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_capacity;
	}

	Expected<gpu::Sampler> BindlessManager::CreateSampler(const gpu::Filter filter, const gpu::SamplerMipmapMode mipmap, const gpu::SamplerAddressMode address) const
	{
		const std::scoped_lock lock(m_mutex);
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
		        .anisotropyEnable = m_maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE,
		        .maxAnisotropy = m_maxAnisotropy,
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
		const std::scoped_lock lock(m_mutex);
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
		const std::scoped_lock lock(m_mutex);
		if (m_device == nullptr)
		{
			return;
		}

		FreeSlotImmediateUnlocked(slot);
	}

	void BindlessManager::FreeSampledImageSlotDeferred(const std::uint32_t slot)
	{
		const std::scoped_lock lock(m_mutex);
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
		AE_PROFILE_ZONE();
		const std::scoped_lock lock(m_mutex);
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

	gpu::DeviceAddress BindlessManager::GetResourceHeapAddress() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_resourceHeapAddr;
	}

	gpu::DeviceAddress BindlessManager::GetSamplerHeapAddress() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_samplerHeapAddr;
	}

	gpu::DeviceSize BindlessManager::GetResourceHeapSize() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_resourceHeapSize;
	}

	gpu::DeviceSize BindlessManager::GetSamplerHeapSize() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_samplerHeapSize;
	}

	gpu::DeviceSize BindlessManager::GetImageDescriptorSize() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_imageDescriptorSize;
	}

	const void* BindlessManager::GetDescriptorHeapMappings() const
	{
		return m_shaderMappingInfo;
	}

	Expected<void> BindlessManager::WriteSampledImage(const std::uint32_t slot, const void* viewCreateInfo, const void* imageView, const gpu::ImageLayout layout)
	{
		const std::scoped_lock lock(m_mutex);
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

		if (!m_useDescriptorHeap)
		{
			if (imageView == nullptr)
			{
				return Unexpected{AetherError::Engine("BindlessManager: the descriptor-buffer backend needs a VkImageView, not just a create-info recipe.")};
			}

			const VkDescriptorImageInfo imageDesc{
			        .sampler = VK_NULL_HANDLE,
			        .imageView = static_cast<VkImageView>(const_cast<void*>(imageView)),
			        .imageLayout = gpu::ToVk(layout),
			};
			const VkDescriptorGetInfoEXT getInfo{
			        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
			        .pNext = nullptr,
			        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			        .data = {.pSampledImage = &imageDesc},
			};

			const VkDeviceSize offset = m_imageBindingOffset + static_cast<VkDeviceSize>(slot) * m_imageDescriptorSize;
			vkGetDescriptorEXT(static_cast<VkDevice>(m_device), &getInfo, m_imageDescriptorSize, static_cast<std::byte*>(m_resourceHeapMapped) + offset);
			const VkResult bufferFlush = vmaFlushAllocation(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VmaAllocation>(m_resourceHeapAlloc), offset, m_imageDescriptorSize);
			if (bufferFlush != VK_SUCCESS)
			{
				return Unexpected{AetherError::Vulkan(static_cast<std::int32_t>(bufferFlush), "BindlessManager: failed to flush a sampled-image descriptor write.")};
			}
			return {};
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
		const std::scoped_lock lock(m_mutex);
		WriteLinearSamplerUnlocked();
	}

	void BindlessManager::WriteLinearSamplerUnlocked()
	{
		if (m_device == nullptr)
		{
			return;
		}
		if (m_useDescriptorHeap && m_samplerHeapMapped == nullptr)
		{
			return;
		}
		if (!m_useDescriptorHeap && m_resourceHeapMapped == nullptr)
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
		        .anisotropyEnable = m_maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE,
		        .maxAnisotropy = m_maxAnisotropy,
		        .compareEnable = VK_FALSE,
		        .minLod = 0.0f,
		        .maxLod = VK_LOD_CLAMP_NONE,
		        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
		        .unnormalizedCoordinates = VK_FALSE,
		};

		if (!m_useDescriptorHeap)
		{
			// vkGetDescriptorEXT takes a live VkSampler, so the fallback owns one for the
			// lifetime of the layout rather than describing it inline like the heap path does.
			if (m_fallbackSampler == nullptr)
			{
				VkSampler sampler = VK_NULL_HANDLE;
				const VkResult created = vkCreateSampler(static_cast<VkDevice>(m_device), &samplerInfo, nullptr, &sampler);
				AE_ASSERT_ALWAYS(created == VK_SUCCESS, "BindlessManager: failed to create the global linear sampler.");
				m_fallbackSampler = static_cast<void*>(sampler);
			}

			const auto sampler = static_cast<VkSampler>(m_fallbackSampler);
			const VkDescriptorGetInfoEXT getInfo{
			        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
			        .pNext = nullptr,
			        .type = VK_DESCRIPTOR_TYPE_SAMPLER,
			        .data = {.pSampler = &sampler},
			};
			vkGetDescriptorEXT(static_cast<VkDevice>(m_device), &getInfo, m_samplerDescriptorSize, static_cast<std::byte*>(m_resourceHeapMapped) + m_samplerBindingOffset);
			const VkResult bufferFlush = vmaFlushAllocation(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VmaAllocation>(m_resourceHeapAlloc), m_samplerBindingOffset, m_samplerDescriptorSize);
			AE_ASSERT_ALWAYS(bufferFlush == VK_SUCCESS, "BindlessManager: failed to flush the sampler descriptor write.");
			return;
		}

		const VkHostAddressRangeEXT hostRange{
		        .address = m_samplerHeapMapped,
		        .size = m_samplerDescriptorSize,
		};

		vkWriteSamplerDescriptorsEXT(static_cast<VkDevice>(m_device), 1, &samplerInfo, &hostRange);
		const VkResult flushResult = vmaFlushAllocation(reinterpret_cast<VmaAllocator>(m_vmaAllocator), static_cast<VmaAllocation>(m_samplerHeapAlloc), 0, m_samplerDescriptorSize);
		AE_ASSERT_ALWAYS(flushResult == VK_SUCCESS, "BindlessManager: failed to flush the sampler descriptor write.");
	}

	bool BindlessManager::UsesDescriptorHeap() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_useDescriptorHeap;
	}

	void BindlessManager::CmdBindGlobalResources(gpu::CommandList& cmd) const
	{
		auto* const vkCmd = static_cast<VkCommandBuffer>(cmd.GetCommandBuffer());

		if (!m_useDescriptorHeap)
		{
			if (m_resourceHeapBuffer == nullptr || m_pipelineLayout == nullptr)
			{
				return;
			}

			const VkDescriptorBufferBindingInfoEXT bindingInfo{
			        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
			        .pNext = nullptr,
			        .address = m_resourceHeapAddr,
			        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			};
			vkCmdBindDescriptorBuffersEXT(vkCmd, 1, &bindingInfo);

			// Set 0 lives at offset 0 of buffer 0. Both bind points get it: a command buffer
			// mixes draws and dispatches, and the offsets are per-bind-point state.
			constexpr std::uint32_t bufferIndex = 0;
			constexpr VkDeviceSize setOffset = 0;
			const auto layout = static_cast<VkPipelineLayout>(m_pipelineLayout);
			vkCmdSetDescriptorBufferOffsetsEXT(vkCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bufferIndex, &setOffset);
			vkCmdSetDescriptorBufferOffsetsEXT(vkCmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &bufferIndex, &setOffset);
			return;
		}

		if (m_resourceHeapBuffer == nullptr || m_samplerHeapBuffer == nullptr)
		{
			return;
		}

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
