#include "vulkan/ResourceRegistry.hpp"

#include <algorithm>

#include "utils/Assert.hpp"
#include "utils/Backtrace.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "gpu/BindlessManager.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "vulkan/ComputePipelineFactory.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "vulkan/GraphicsPipelineFactory.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	namespace
	{
		inline constexpr std::uint32_t kIndexInvalid = 0x0000FFFFu;
		inline constexpr std::uint32_t kGenerationInvalid = 0u;
		inline constexpr std::uint32_t kGenerationWrap = 65536u;

#ifndef NDEBUG
		template<typename Slot>
		static std::string FormatAllocFrames(const Slot& slot) noexcept
		{
			if (slot.allocSiteCount == 0)
			{
				return {};
			}

			std::string result;
			const int start = std::max(0, slot.allocSiteCount - ResourceRegistry::kAllocFrames);
			for (int i = start; i < slot.allocSiteCount; i++)
			{
				const auto& frame = slot.allocFrames[i % ResourceRegistry::kAllocFrames];
				if (frame.site.line() == 0)
				{
					continue;
				}
				result += "\n  #";
				result += std::to_string(i);
				result += ": ";
				result += ShortenPath(frame.site.file_name());
				result += ":";
				result += std::to_string(frame.site.line());

				for (int f = 0; f < frame.frameCount; f++)
				{
					std::string trace = ResolveAddress(frame.addresses[f]);
					if (!trace.empty())
					{
						result += "\n    ";
						result += trace;
					}
				}
			}
			return result;
		}
#endif

		[[nodiscard]] VmaAllocationCreateInfo MakeMappedAllocInfo(gpu::MappedMemoryUsage memUsage) noexcept
		{
			VmaAllocationCreateInfo info{};
			switch (memUsage)
			{
				case gpu::MappedMemoryUsage::GpuToCpu:
					info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
					info.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
					break;
				case gpu::MappedMemoryUsage::CpuToGpu:
					info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
					info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
					break;
				case gpu::MappedMemoryUsage::Auto:
				default:
					info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
					info.usage = VMA_MEMORY_USAGE_AUTO;
					break;
			}
			return info;
		}

		[[nodiscard]] VmaAllocationCreateInfo MakeDeviceLocalAllocInfo() noexcept
		{
			VmaAllocationCreateInfo info{};
			info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			return info;
		}

		[[nodiscard]] VkImageAspectFlags DeduceAspect(VkFormat format)
		{
#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wswitch-enum"
#endif
			switch (format)
			{
				case VK_FORMAT_S8_UINT:
					return VK_IMAGE_ASPECT_STENCIL_BIT;
				case VK_FORMAT_D16_UNORM:
				case VK_FORMAT_D32_SFLOAT:
				case VK_FORMAT_X8_D24_UNORM_PACK32:
					return VK_IMAGE_ASPECT_DEPTH_BIT;
				case VK_FORMAT_D16_UNORM_S8_UINT:
				case VK_FORMAT_D24_UNORM_S8_UINT:
				case VK_FORMAT_D32_SFLOAT_S8_UINT:
					return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
				default:
					return VK_IMAGE_ASPECT_COLOR_BIT;
			}
#ifdef __clang__
#	pragma clang diagnostic pop
#endif
		}
	} // namespace

	ResourceRegistry::~ResourceRegistry()
	{
		Shutdown();
	}

	void ResourceRegistry::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_shutdown)
		{
			return;
		}
		m_shutdown = true;

		DrainAll();

		for (auto& slot: m_textures)
		{
			if (slot.entry)
			{
#ifndef NDEBUG
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked texture '{}' (gen {}).{}", slot.debugName, slot.generation, FormatAllocFrames(slot));
#else
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked texture '{}' (gen {}).", slot.debugName, slot.generation);
#endif
				if (slot.entry->hasBindlessSampled && m_bindlessManager)
				{
					m_bindlessManager->FreeSampledImageSlot(slot.entry->bindlessSampledSlot);
				}
				DestroyTextureEntryNow(*slot.entry);
				--m_liveTextureCount;
				slot.entry.reset();
			}
		}
		for (auto& slot: m_buffers)
		{
			if (slot.entry)
			{
#ifndef NDEBUG
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked buffer '{}' (gen {}).{}", slot.debugName, slot.generation, FormatAllocFrames(slot));
#else
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked buffer '{}' (gen {}).", slot.debugName, slot.generation);
#endif
				DestroyBufferEntryNow(*slot.entry, m_memoryTracker);
				--m_liveBufferCount;
				slot.entry.reset();
			}
		}
		for (auto& slot: m_pipelines)
		{
			if (slot.entry)
			{
#ifndef NDEBUG
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked pipeline '{}' (gen {}).{}", slot.debugName, slot.generation, FormatAllocFrames(slot));
#else
				AE_WARN(LogCategory::Vulkan, "ResourceRegistry::Shutdown: leaked pipeline '{}' (gen {}).", slot.debugName, slot.generation);
#endif
				DestroyPipelineEntryNow(*slot.entry);
				--m_livePipelineCount;
				slot.entry.reset();
			}
		}
	}

	void ResourceRegistry::Init(VkDevice device, VmaAllocator allocator) noexcept
	{
		AE_PROFILE_ZONE();
		m_device = device;
		m_allocator = allocator;
		m_textures.reserve(1024);
		m_buffers.reserve(1024);
		m_pipelines.reserve(256);
	}

	void ResourceRegistry::SetSharedBufferQueueFamilies(std::initializer_list<std::uint32_t> families) noexcept
	{
		m_sharedBufferFamilyCount = 0;
		for (const std::uint32_t family: families)
		{
			bool known = false;
			for (std::uint32_t i = 0; i < m_sharedBufferFamilyCount; ++i)
			{
				known = known || m_sharedBufferFamilies[i] == family;
			}
			if (!known && m_sharedBufferFamilyCount < 3)
			{
				m_sharedBufferFamilies[m_sharedBufferFamilyCount++] = family;
			}
		}
		if (m_sharedBufferFamilyCount < 2)
		{
			m_sharedBufferFamilyCount = 0; // single family: keep buffers exclusive
		}
	}

	gpu::BufferHandle ResourceRegistry::CreateBuffer(const gpu::BufferDesc& desc, std::source_location loc) noexcept
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkBufferUsageFlags2 vkUsage = gpu::ToVk(desc.usage);

		const VkBufferUsageFlags2CreateInfo usageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = vkUsage,
		};

		VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageFlags2,
		        .size = desc.size,
		        .usage = 0,
		};
		if (m_sharedBufferFamilyCount > 1)
		{
			bufInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
			bufInfo.queueFamilyIndexCount = m_sharedBufferFamilyCount;
			bufInfo.pQueueFamilyIndices = m_sharedBufferFamilies;
		}

		const VmaAllocationCreateInfo allocInfo = MakeDeviceLocalAllocInfo();

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo allocResult{};

		const VkResult result = vmaCreateBuffer(m_allocator, &bufInfo, &allocInfo, &buffer, &allocation, &allocResult);
		if (result != VK_SUCCESS)
		{
			return {};
		}

		gpu::DeviceAddress deviceAddress = 0;
		if ((desc.usage & gpu::BufferUsage::ShaderDeviceAddress) != gpu::BufferUsage::None)
		{
			const VkBufferDeviceAddressInfo addrInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = buffer,
			};
			deviceAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
			if (m_memoryTracker != nullptr && deviceAddress != 0)
			{
				m_memoryTracker->Register(deviceAddress, desc.size, std::string(desc.debugName ? desc.debugName : "<buffer>"), GpuMemoryTracker::ResourceType::Buffer);
			}
		}

		BufferEntry entry{};
		entry.buffer = buffer;
		entry.device = m_device;
		entry.allocation = allocation;
		entry.allocator = m_allocator;
		entry.usage = bufInfo.usage;
		entry.size = desc.size;
		entry.ownsAllocation = true;
		entry.deviceAddress = deviceAddress;

		const gpu::BufferHandle handle = RegisterBuffer(entry, desc.debugName ? std::string_view(desc.debugName) : std::string_view{}, loc);
		if (!handle.IsValid())
		{
			vmaDestroyBuffer(m_allocator, buffer, allocation);
			return {};
		}
		return handle;
	}

	gpu::BufferHandle ResourceRegistry::CreateMappedBuffer(const gpu::MappedBufferDesc& desc, std::source_location loc) noexcept
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkBufferUsageFlags2 vkUsage = gpu::ToVk(desc.usage);

		const VkBufferUsageFlags2CreateInfo usageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = vkUsage,
		};

		VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageFlags2,
		        .size = desc.size,
		        .usage = 0,
		};
		if (m_sharedBufferFamilyCount > 1)
		{
			bufInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
			bufInfo.queueFamilyIndexCount = m_sharedBufferFamilyCount;
			bufInfo.pQueueFamilyIndices = m_sharedBufferFamilies;
		}

		const VmaAllocationCreateInfo allocInfo = MakeMappedAllocInfo(desc.memoryUsage);

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo allocResult{};

		const VkResult result = vmaCreateBuffer(m_allocator, &bufInfo, &allocInfo, &buffer, &allocation, &allocResult);
		if (result != VK_SUCCESS)
		{
			return {};
		}

		gpu::DeviceAddress deviceAddress = 0;
		if ((desc.usage & gpu::BufferUsage::ShaderDeviceAddress) != gpu::BufferUsage::None)
		{
			const VkBufferDeviceAddressInfo addrInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = buffer,
			};
			deviceAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
			if (m_memoryTracker != nullptr && deviceAddress != 0)
			{
				m_memoryTracker->Register(deviceAddress, desc.size, std::string(desc.debugName ? desc.debugName : "<mapped_buffer>"), GpuMemoryTracker::ResourceType::Buffer);
			}
		}

		BufferEntry entry{};
		entry.buffer = buffer;
		entry.device = m_device;
		entry.allocation = allocation;
		entry.allocator = m_allocator;
		entry.usage = vkUsage;
		entry.size = desc.size;
		entry.ownsAllocation = true;
		entry.mappedPtr = allocResult.pMappedData;
		entry.deviceAddress = deviceAddress;

		if (desc.debugName != nullptr)
		{
			vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(entry.buffer), VK_OBJECT_TYPE_BUFFER, desc.debugName);
		}

		const gpu::BufferHandle handle = RegisterBuffer(entry, desc.debugName ? std::string_view(desc.debugName) : std::string_view{}, loc);
		if (!handle.IsValid())
		{
			vmaDestroyBuffer(m_allocator, buffer, allocation);
			return {};
		}

		return handle;
	}

	gpu::TextureHandle ResourceRegistry::CreateTexture(const gpu::TextureDesc& desc, std::source_location loc) noexcept
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkImageCreateInfo imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = gpu::ToVk(desc.format),
		        .extent = {.width = desc.extent.width, .height = desc.extent.height, .depth = 1u},
		        .mipLevels = desc.mipLevels,
		        .arrayLayers = desc.arrayLayers,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = gpu::ToVk(desc.usage),
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		const VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		};

		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo allocResult{};

		const VkResult createResult = vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &image, &allocation, &allocResult);
		if (createResult != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "ResourceRegistry::CreateTexture: vmaCreateImage failed (VkResult={}).", static_cast<int32_t>(createResult));
			return {};
		}

		const VkImageViewCreateInfo viewInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		        .image = image,
		        .viewType = desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		        .format = imageInfo.format,
		        .components =
		                {
		                        .r = gpu::ToVk(desc.r),
		                        .g = gpu::ToVk(desc.g),
		                        .b = gpu::ToVk(desc.b),
		                        .a = gpu::ToVk(desc.a),
		                },
		        .subresourceRange = {.aspectMask = gpu::ToVk(desc.aspect), .baseMipLevel = 0, .levelCount = desc.mipLevels, .baseArrayLayer = 0, .layerCount = desc.arrayLayers},
		};
		VkImageView view = VK_NULL_HANDLE;
		const VkResult viewResult = vkCreateImageView(m_device, &viewInfo, nullptr, &view);
		if (viewResult != VK_SUCCESS)
		{
			vmaDestroyImage(m_allocator, image, allocation);
			AE_ERROR(LogCategory::Vulkan, "ResourceRegistry::CreateTexture: vkCreateImageView failed (VkResult={}).", static_cast<int32_t>(viewResult));
			return {};
		}

		TextureEntry entry{};
		entry.image = image;
		entry.view = view;
		entry.storageView = VK_NULL_HANDLE;
		entry.format = imageInfo.format;
		entry.extent = {.width = desc.extent.width, .height = desc.extent.height};
		entry.usage = imageInfo.usage;
		entry.aspect = gpu::ToVk(desc.aspect);
		entry.device = m_device;
		entry.allocation = allocation;
		entry.allocator = m_allocator;
		entry.ownsAllocation = true;
		entry.mipLevels = desc.mipLevels;
		entry.arrayLayers = desc.arrayLayers;
		entry.viewCreateInfo = viewInfo;

		const gpu::TextureHandle handle = RegisterTexture(entry, desc.debugName ? std::string_view(desc.debugName) : std::string_view{}, loc);
		if (!handle.IsValid())
		{
			vkDestroyImageView(m_device, view, nullptr);
			vmaDestroyImage(m_allocator, image, allocation);
			return {};
		}

		return handle;
	}

	gpu::MappedBufferView ResourceRegistry::ResolveMappedBuffer(gpu::BufferHandle handle) const noexcept
	{
		if (!handle.IsValid())
		{
			return {};
		}

		const auto* const entry = Resolve(handle);
		if (!entry)
		{
			return {};
		}

		return gpu::MappedBufferView{
		        .mappedPtr = entry->mappedPtr,
		        .deviceAddress = entry->deviceAddress,
		        .size = entry->size,
		};
	}

	void ResourceRegistry::FlushMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept
	{
		if (!handle.IsValid())
		{
			return;
		}

		const auto* const entry = Resolve(handle);
		if (!entry || entry->allocation == VK_NULL_HANDLE)
		{
			return;
		}

		vmaFlushAllocation(m_allocator, entry->allocation, offset, size);
	}

	void ResourceRegistry::InvalidateMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept
	{
		if (!handle.IsValid())
		{
			return;
		}

		const auto* const entry = Resolve(handle);
		if (!entry || entry->allocation == VK_NULL_HANDLE)
		{
			return;
		}

		vmaInvalidateAllocation(m_allocator, entry->allocation, offset, size);
	}

	void ResourceRegistry::SetBufferName(gpu::BufferHandle handle, const char* name)
	{
		const BufferEntry* entry = Resolve(handle);
		if (!entry || entry->buffer == VK_NULL_HANDLE || !name)
		{
			return;
		}
		vkutil::SetObjectName(entry->device, reinterpret_cast<std::uint64_t>(entry->buffer), VK_OBJECT_TYPE_BUFFER, name);
	}

	void ResourceRegistry::SetTextureName(gpu::TextureHandle handle, const char* name)
	{
		const TextureEntry* entry = Resolve(handle);
		if (!entry || entry->image == VK_NULL_HANDLE || !name)
		{
			return;
		}
		vkutil::SetObjectName(entry->device, reinterpret_cast<std::uint64_t>(entry->image), VK_OBJECT_TYPE_IMAGE, name);
		if (entry->view != VK_NULL_HANDLE)
		{
			const std::string viewName = std::string(name) + ".View";
			vkutil::SetObjectName(entry->device, reinterpret_cast<std::uint64_t>(entry->view), VK_OBJECT_TYPE_IMAGE_VIEW, viewName.c_str());
		}
	}

	void ResourceRegistry::SetBindlessManager(BindlessManager* mgr)
	{
		m_bindlessManager = mgr;
	}

	void ResourceRegistry::SetMemoryTracker(GpuMemoryTracker* tracker)
	{
		m_memoryTracker = tracker;
	}

	gpu::BufferHandle ResourceRegistry::CreateAliasedBuffer(VkDeviceSize size, VkBufferUsageFlags2 usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName)
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkBufferUsageFlags2CreateInfo usageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = usage,
		};

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageFlags2,
		        .size = size,
		        .usage = 0,
		};

		VkBuffer buffer = VK_NULL_HANDLE;
		VkResult result = vkCreateBuffer(m_device, &bufInfo, nullptr, &buffer);
		if (result != VK_SUCCESS)
		{
			return {};
		}

		VkMemoryRequirements memReq;
		vkGetBufferMemoryRequirements(m_device, buffer, &memReq);
		AE_ASSERT((memoryOffset % memReq.alignment) == 0, "CreateAliasedBuffer: memoryOffset is not aligned to VkMemoryRequirements::alignment.");

		VmaAllocationInfo existingAllocInfo;
		vmaGetAllocationInfo(m_allocator, existingAllocation, &existingAllocInfo);

		const VkBindBufferMemoryInfo bindInfo{
		        .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
		        .buffer = buffer,
		        .memory = existingAllocInfo.deviceMemory,
		        .memoryOffset = memoryOffset,
		};
		result = vkBindBufferMemory2(m_device, 1, &bindInfo);
		if (result != VK_SUCCESS)
		{
			vkDestroyBuffer(m_device, buffer, nullptr);
			return {};
		}

		BufferEntry entry{};
		entry.buffer = buffer;
		entry.device = m_device;
		entry.allocation = existingAllocation;
		entry.allocator = m_allocator;
		entry.usage = usage;
		entry.size = size;
		entry.ownsAllocation = false;

		if ((usage & VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT) != 0)
		{
			const VkBufferDeviceAddressInfo addrInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = buffer,
			};
			entry.deviceAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
			if (m_memoryTracker != nullptr && entry.deviceAddress != 0)
			{
				m_memoryTracker->Register(entry.deviceAddress, size, debugName.empty() ? std::string{"<aliased_buffer>"} : std::string(debugName), GpuMemoryTracker::ResourceType::Buffer);
			}
		}

		return RegisterBuffer(entry, debugName.empty() ? std::string_view{} : debugName);
	}

	gpu::TextureHandle ResourceRegistry::CreateAliasedTexture(const gpu::TextureDesc& desc, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName)
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkImageCreateInfo imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .flags = VK_IMAGE_CREATE_ALIAS_BIT,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = gpu::ToVk(desc.format),
		        .extent = {.width = desc.extent.width, .height = desc.extent.height, .depth = 1u},
		        .mipLevels = desc.mipLevels,
		        .arrayLayers = desc.arrayLayers,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = gpu::ToVk(desc.usage),
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		VkImage image = VK_NULL_HANDLE;
		VkResult result = vkCreateImage(m_device, &imageInfo, nullptr, &image);
		if (result != VK_SUCCESS)
		{
			return {};
		}

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(m_device, image, &memReq);
		AE_ASSERT((memoryOffset % memReq.alignment) == 0, "CreateAliasedTexture: memoryOffset is not aligned to VkMemoryRequirements::alignment.");

		VmaAllocationInfo existingAllocInfo;
		vmaGetAllocationInfo(m_allocator, existingAllocation, &existingAllocInfo);

		const VkBindImageMemoryInfo bindInfo{
		        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
		        .image = image,
		        .memory = existingAllocInfo.deviceMemory,
		        .memoryOffset = memoryOffset,
		};
		result = vkBindImageMemory2(m_device, 1, &bindInfo);
		if (result != VK_SUCCESS)
		{
			vkDestroyImage(m_device, image, nullptr);
			return {};
		}

		const VkImageAspectFlags aspect = (desc.aspect != gpu::ImageAspect::None) ? gpu::ToVk(desc.aspect) : DeduceAspect(imageInfo.format);
		const VkImageViewCreateInfo viewInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		        .image = image,
		        .viewType = desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		        .format = imageInfo.format,
		        .components =
		                {
		                        .r = gpu::ToVk(desc.r),
		                        .g = gpu::ToVk(desc.g),
		                        .b = gpu::ToVk(desc.b),
		                        .a = gpu::ToVk(desc.a),
		                },
		        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = desc.mipLevels, .baseArrayLayer = 0, .layerCount = desc.arrayLayers},
		};

		VkImageView view = VK_NULL_HANDLE;
		result = vkCreateImageView(m_device, &viewInfo, nullptr, &view);
		if (result != VK_SUCCESS)
		{
			vkDestroyImage(m_device, image, nullptr);
			return {};
		}

		TextureEntry entry{};
		entry.image = image;
		entry.view = view;
		entry.storageView = VK_NULL_HANDLE;
		entry.format = imageInfo.format;
		entry.extent = {.width = desc.extent.width, .height = desc.extent.height};
		entry.usage = imageInfo.usage;
		entry.aspect = aspect;
		entry.device = m_device;
		entry.allocation = existingAllocation;
		entry.allocator = m_allocator;
		entry.ownsAllocation = false;
		entry.mipLevels = desc.mipLevels;
		entry.arrayLayers = desc.arrayLayers;
		entry.viewCreateInfo = viewInfo;

		return RegisterTexture(entry, debugName.empty() ? std::string_view{} : debugName);
	}

	Expected<void> ResourceRegistry::EnsureBindlessSampled(gpu::TextureHandle handle, const gpu::ImageAspect aspectMask, const gpu::ImageLayout descriptorLayout)
	{
		AE_ASSERT(m_bindlessManager != nullptr, "EnsureBindlessSampled: SetBindlessManager was never called.");

		TextureEntry* entry = ResolveMutable(handle);
		if (!entry)
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "EnsureBindlessSampled: invalid handle"));
		}

		if (entry->hasBindlessSampled)
		{
			return {};
		}

		if (entry->image == VK_NULL_HANDLE)
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "EnsureBindlessSampled: no image backing handle"));
		}

		const bool makeView = (entry->view == VK_NULL_HANDLE);
		VkImageView view = entry->view;

		if (makeView)
		{
			const VkImageViewCreateInfo viewCreateInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			        .image = entry->image,
			        .viewType = entry->arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
			        .format = entry->format,
			        .subresourceRange =
			                {
			                        .aspectMask = gpu::ToVk(aspectMask),
			                        .baseMipLevel = 0,
			                        .levelCount = entry->mipLevels,
			                        .baseArrayLayer = 0,
			                        .layerCount = entry->arrayLayers,
			                },
			};
			const VkResult viewResult = vkCreateImageView(entry->device, &viewCreateInfo, nullptr, &view);
			if (viewResult != VK_SUCCESS)
			{
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(viewResult), "Failed to create image view for bindless registration"));
			}
			entry->viewCreateInfo = viewCreateInfo;
		}

		Expected<std::uint32_t> slotResult = m_bindlessManager->AllocateSampledImageSlot();
		if (!slotResult)
		{
			if (makeView)
			{
				vkDestroyImageView(entry->device, view, nullptr);
			}
			AE_UNEXPECTED(slotResult.error());
		}

		Expected<void> writeResult = m_bindlessManager->WriteSampledImage(*slotResult, &entry->viewCreateInfo, descriptorLayout);
		if (!writeResult)
		{
			m_bindlessManager->FreeSampledImageSlot(*slotResult);
			if (makeView)
			{
				vkDestroyImageView(entry->device, view, nullptr);
			}
			AE_UNEXPECTED(writeResult.error());
		}

		entry->view = view;
		if (makeView)
		{
			entry->ownsView = true;
		}
		entry->bindlessSampledSlot = *slotResult;
		entry->hasBindlessSampled = true;
		return {};
	}

	bool ResourceRegistry::HasBindlessSampled(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? entry->hasBindlessSampled : false;
	}

	std::uint32_t ResourceRegistry::GetBindlessSampledSlot(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? entry->bindlessSampledSlot : TextureEntry::kInvalidBindlessSlot;
	}

	gpu::Format ResourceRegistry::GetTextureFormat(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? gpu::FromVk(entry->format) : gpu::Format::Undefined;
	}

	gpu::Extent2D ResourceRegistry::GetTextureExtent(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? gpu::Extent2D{entry->extent.width, entry->extent.height} : gpu::Extent2D{};
	}

	std::uint32_t ResourceRegistry::GetTextureMipLevels(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? entry->mipLevels : 0;
	}

	std::uint32_t ResourceRegistry::GetTextureArrayLayers(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? entry->arrayLayers : 0;
	}

	gpu::ImageUsage ResourceRegistry::GetTextureUsage(gpu::TextureHandle handle) const
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? static_cast<gpu::ImageUsage>(entry->usage) : gpu::ImageUsage::None;
	}

	std::vector<gpu::DebugTextureInfo> ResourceRegistry::ListDebugTextures() const
	{
		std::vector<gpu::DebugTextureInfo> result;
		result.reserve(m_liveTextureCount);
		for (std::uint32_t i = 0; i < m_textures.size(); ++i)
		{
			const TextureSlot& slot = m_textures[i];
			if (!slot.entry.has_value())
			{
				continue;
			}

			const TextureEntry& entry = *slot.entry;
			result.push_back(gpu::DebugTextureInfo{
			        .handle = gpu::TextureHandle::Make(i, slot.generation),
			        .view = static_cast<gpu::ImageView>(entry.view),
			        .format = gpu::FromVk(entry.format),
			        .extent = gpu::Extent2D{entry.extent.width, entry.extent.height},
			        .usage = static_cast<gpu::ImageUsage>(entry.usage),
			        .aspect = static_cast<gpu::ImageAspect>(entry.aspect),
			        .mipLevels = entry.mipLevels,
			        .arrayLayers = entry.arrayLayers,
			        .hasBindlessSampled = entry.hasBindlessSampled,
			        .bindlessSampledSlot = entry.bindlessSampledSlot,
			        .debugName = slot.debugName,
			});
		}
		return result;
	}

	gpu::DeviceSize ResourceRegistry::GetBufferSize(gpu::BufferHandle handle) const
	{
		const BufferEntry* entry = Resolve(handle);
		return entry ? entry->size : 0;
	}

	gpu::BufferUsage ResourceRegistry::GetBufferUsage(gpu::BufferHandle handle) const
	{
		const BufferEntry* entry = Resolve(handle);
		return entry ? static_cast<gpu::BufferUsage>(entry->usage) : gpu::BufferUsage::None;
	}

	const void* ResourceRegistry::GetViewCreateInfo(gpu::TextureHandle handle) const noexcept
	{
		const TextureEntry* entry = Resolve(handle);
		return entry ? &entry->viewCreateInfo : nullptr;
	}

	std::uint32_t ResourceRegistry::AcquireTextureSlot()
	{
		if (!m_freeTextureSlots.empty())
		{
			const std::uint32_t i = m_freeTextureSlots.back();
			m_freeTextureSlots.pop_back();
			return i;
		}
		if (m_textures.size() < kIndexInvalid)
		{
			const auto i = static_cast<std::uint32_t>(m_textures.size());
			m_textures.push_back(TextureSlot{});
			return i;
		}
		AE_ASSERT(false, "ResourceRegistry: out of TextureSlot indices (16-bit index space exhausted).");
		return kIndexInvalid;
	}

	std::uint32_t ResourceRegistry::AcquireBufferSlot()
	{
		if (!m_freeBufferSlots.empty())
		{
			const std::uint32_t i = m_freeBufferSlots.back();
			m_freeBufferSlots.pop_back();
			return i;
		}
		if (m_buffers.size() < kIndexInvalid)
		{
			const auto i = static_cast<std::uint32_t>(m_buffers.size());
			m_buffers.push_back(BufferSlot{});
			return i;
		}
		AE_ASSERT(false, "ResourceRegistry: out of BufferSlot indices (16-bit index space exhausted).");
		return kIndexInvalid;
	}

	std::uint32_t ResourceRegistry::AcquirePipelineSlot()
	{
		if (!m_freePipelineSlots.empty())
		{
			const std::uint32_t i = m_freePipelineSlots.back();
			m_freePipelineSlots.pop_back();
			return i;
		}
		if (m_pipelines.size() < kIndexInvalid)
		{
			const auto i = static_cast<std::uint32_t>(m_pipelines.size());
			m_pipelines.push_back(PipelineSlot{});
			return i;
		}
		AE_ASSERT(false, "ResourceRegistry: out of PipelineSlot indices (16-bit index space exhausted).");
		return kIndexInvalid;
	}

	gpu::TextureHandle ResourceRegistry::RegisterTexture(const TextureEntry& entry, std::string_view debugName, std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterTexture called after Shutdown.");
		const std::uint32_t idx = AcquireTextureSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_liveTextureCount;
		m_textures[idx].entry = entry;
		{
			auto nameStr = debugName.empty() ? std::to_string(idx) : std::string(debugName);
			nameStr += " (" + std::string(loc.file_name()) + ":" + std::to_string(loc.line()) + ")";
			m_textures[idx].debugName = std::move(nameStr);
		}
#ifndef NDEBUG
		{
			auto& s = m_textures[idx];
			const int i = s.allocSiteCount % kAllocFrames;
			s.allocFrames[i].site = loc;
			s.allocFrames[i].frameCount = aether::CaptureBacktrace(s.allocFrames[i].addresses.data(), kBacktraceDepth, 1);
			s.allocSiteCount++;
		}
#endif
		const std::uint32_t gen = m_textures[idx].generation;
		return gpu::TextureHandle::Make(idx, gen);
	}

	gpu::BufferHandle ResourceRegistry::RegisterBuffer(const BufferEntry& entry, std::string_view debugName, std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterBuffer called after Shutdown.");
		const std::uint32_t idx = AcquireBufferSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_liveBufferCount;
		m_buffers[idx].entry = entry;
		{
			auto nameStr = debugName.empty() ? std::to_string(idx) : std::string(debugName);
			nameStr += " (" + std::string(loc.file_name()) + ":" + std::to_string(loc.line()) + ")";
			m_buffers[idx].debugName = std::move(nameStr);
		}
#ifndef NDEBUG
		{
			auto& s = m_buffers[idx];
			const int i = s.allocSiteCount % kAllocFrames;
			s.allocFrames[i].site = loc;
			s.allocFrames[i].frameCount = aether::CaptureBacktrace(s.allocFrames[i].addresses.data(), kBacktraceDepth, 1);
			s.allocSiteCount++;
		}
#endif
		const std::uint32_t gen = m_buffers[idx].generation;
		return gpu::BufferHandle::Make(idx, gen);
	}

	gpu::PipelineHandle ResourceRegistry::RegisterPipeline(const PipelineEntry& entry, std::string_view debugName, std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterPipeline called after Shutdown.");
		const std::uint32_t idx = AcquirePipelineSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_livePipelineCount;
		m_pipelines[idx].entry = entry;
		{
			auto nameStr = debugName.empty() ? std::to_string(idx) : std::string(debugName);
			nameStr += " (" + std::string(loc.file_name()) + ":" + std::to_string(loc.line()) + ")";
			m_pipelines[idx].debugName = std::move(nameStr);
		}
#ifndef NDEBUG
		{
			auto& s = m_pipelines[idx];
			const int i = s.allocSiteCount % kAllocFrames;
			s.allocFrames[i].site = loc;
			s.allocFrames[i].frameCount = aether::CaptureBacktrace(s.allocFrames[i].addresses.data(), kBacktraceDepth, 1);
			s.allocSiteCount++;
		}
#endif
		const std::uint32_t gen = m_pipelines[idx].generation;
		return gpu::PipelineHandle::Make(idx, gen);
	}

	void ResourceRegistry::Destroy(gpu::TextureHandle handle)
	{
		if (!handle.IsValid())
		{
			return;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_textures.size())
		{
			return;
		}
		auto& slot = m_textures[idx];
		if (slot.generation != handle.GetGeneration() || !slot.entry)
		{
			return;
		}
		const TextureEntry entry = *slot.entry;
		--m_liveTextureCount;
		slot.entry.reset();
		slot.generation = (slot.generation + 1u) % kGenerationWrap;
		if (slot.generation == kGenerationInvalid)
		{
			slot.generation = 1u;
		}
		m_freeTextureSlots.push_back(idx);
		m_pendingDestructions[m_currentFrame].push_back(PendingDestruction{
		        .fn =
		                [this, entry]()
		        {
			        if (entry.hasBindlessSampled && m_bindlessManager)
			        {
				        m_bindlessManager->FreeSampledImageSlotDeferred(entry.bindlessSampledSlot);
			        }
			        DestroyTextureEntryNow(entry);
		        },
		});
	}

	void ResourceRegistry::Destroy(gpu::BufferHandle handle)
	{
		if (!handle.IsValid())
		{
			return;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_buffers.size())
		{
			return;
		}
		auto& slot = m_buffers[idx];
		if (slot.generation != handle.GetGeneration() || !slot.entry)
		{
			return;
		}
		const BufferEntry entry = *slot.entry;
		--m_liveBufferCount;
		slot.entry.reset();
		slot.generation = (slot.generation + 1u) % kGenerationWrap;
		if (slot.generation == kGenerationInvalid)
		{
			slot.generation = 1u;
		}
		m_freeBufferSlots.push_back(idx);
		m_pendingDestructions[m_currentFrame].push_back(PendingDestruction{
		        .fn = [this, entry]() { DestroyBufferEntryNow(entry, m_memoryTracker); },
		});
	}

	void ResourceRegistry::Destroy(gpu::PipelineHandle handle)
	{
		if (!handle.IsValid())
		{
			return;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_pipelines.size())
		{
			return;
		}
		auto& slot = m_pipelines[idx];
		if (slot.generation != handle.GetGeneration() || !slot.entry)
		{
			return;
		}
		const PipelineEntry entry = *slot.entry;
		--m_livePipelineCount;
		slot.entry.reset();
		slot.generation = (slot.generation + 1u) % kGenerationWrap;
		if (slot.generation == kGenerationInvalid)
		{
			slot.generation = 1u;
		}
		m_freePipelineSlots.push_back(idx);
		m_pendingDestructions[m_currentFrame].push_back(PendingDestruction{
		        .fn = [this, entry]() { DestroyPipelineEntryNow(entry); },
		});
	}

	const ResourceRegistry::TextureEntry* ResourceRegistry::Resolve(gpu::TextureHandle handle) const
	{
		if (!handle.IsValid())
		{
			return nullptr;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_textures.size())
		{
			return nullptr;
		}
		const auto& slot = m_textures[idx];
		if (slot.generation != handle.GetGeneration())
		{
			return nullptr;
		}
		if (!slot.entry)
		{
#ifndef NDEBUG
			AE_WARN(LogCategory::Vulkan, "Resolve: handle index={} gen={} points to destroyed slot '{}' - use-after-free.", idx, handle.GetGeneration(), m_textures[idx].debugName);
#endif
			return nullptr;
		}
		return &*slot.entry;
	}

	const ResourceRegistry::BufferEntry* ResourceRegistry::Resolve(gpu::BufferHandle handle) const
	{
		if (!handle.IsValid())
		{
			return nullptr;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_buffers.size())
		{
			return nullptr;
		}
		const auto& slot = m_buffers[idx];
		if (slot.generation != handle.GetGeneration())
		{
			return nullptr;
		}
		if (!slot.entry)
		{
#ifndef NDEBUG
			AE_WARN(LogCategory::Vulkan, "Resolve: handle index={} gen={} points to destroyed buffer slot '{}' - use-after-free.", idx, handle.GetGeneration(), m_buffers[idx].debugName);
#endif
			return nullptr;
		}
		return &*slot.entry;
	}

	const ResourceRegistry::PipelineEntry* ResourceRegistry::Resolve(gpu::PipelineHandle handle) const
	{
		if (!handle.IsValid())
		{
			return nullptr;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_pipelines.size())
		{
			return nullptr;
		}
		const auto& slot = m_pipelines[idx];
		if (slot.generation != handle.GetGeneration())
		{
			return nullptr;
		}
		if (!slot.entry)
		{
#ifndef NDEBUG
			AE_WARN(LogCategory::Vulkan, "Resolve: handle index={} gen={} points to destroyed pipeline slot '{}' - use-after-free.", idx, handle.GetGeneration(), m_pipelines[idx].debugName);
#endif
			return nullptr;
		}
		return &*slot.entry;
	}

	ResourceRegistry::TextureEntry* ResourceRegistry::ResolveMutable(gpu::TextureHandle handle)
	{
		if (!handle.IsValid())
		{
			return nullptr;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_textures.size())
		{
			return nullptr;
		}
		auto& slot = m_textures[idx];
		if (slot.generation != handle.GetGeneration())
		{
			return nullptr;
		}
		if (!slot.entry)
		{
#ifndef NDEBUG
			AE_WARN(LogCategory::Vulkan, "ResolveMutable: handle index={} gen={} points to destroyed slot '{}' - use-after-free.", idx, handle.GetGeneration(), m_textures[idx].debugName);
#endif
			return nullptr;
		}
		return &*slot.entry;
	}

	ResourceRegistry::BufferEntry* ResourceRegistry::ResolveMutable(gpu::BufferHandle handle)
	{
		if (!handle.IsValid())
		{
			return nullptr;
		}
		const std::uint32_t idx = handle.GetIndex();
		if (idx >= m_buffers.size())
		{
			return nullptr;
		}
		auto& slot = m_buffers[idx];
		if (slot.generation != handle.GetGeneration())
		{
			return nullptr;
		}
		if (!slot.entry)
		{
#ifndef NDEBUG
			AE_WARN(LogCategory::Vulkan, "ResolveMutable: handle index={} gen={} points to destroyed buffer slot '{}' - use-after-free.", idx, handle.GetGeneration(), m_buffers[idx].debugName);
#endif
			return nullptr;
		}
		return &*slot.entry;
	}

	void ResourceRegistry::AdvanceFrame()
	{
		AE_PROFILE_ZONE();
		const std::uint32_t nextFrame = (m_currentFrame + 1u) % kMaxFramesInFlight;
		RunDestroyersInRing(m_pendingDestructions[nextFrame]);
		m_currentFrame = nextFrame;
	}

	void ResourceRegistry::DrainAll()
	{
		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			RunDestroyersInRing(m_pendingDestructions[i]);
		}
	}

	void ResourceRegistry::RunDestroyersInRing(std::vector<PendingDestruction>& ring)
	{
		std::vector<PendingDestruction> local;
		local.swap(ring);
		for (auto& d: local)
		{
			if (d.fn)
			{
				d.fn();
			}
		}
	}

	void ResourceRegistry::DestroyTextureEntryNow(const TextureEntry& entry)
	{
		if (entry.view != VK_NULL_HANDLE && entry.device != VK_NULL_HANDLE && entry.ownsView)
		{
			vkDestroyImageView(entry.device, entry.view, nullptr);
		}
		if (entry.storageView != VK_NULL_HANDLE && entry.device != VK_NULL_HANDLE && entry.ownsStorageView)
		{
			vkDestroyImageView(entry.device, entry.storageView, nullptr);
		}
		if (entry.image != VK_NULL_HANDLE && entry.ownsAllocation)
		{
			if (entry.allocation != VK_NULL_HANDLE)
			{
				vmaDestroyImage(entry.allocator, entry.image, entry.allocation);
			}
		}
		else if (entry.image != VK_NULL_HANDLE && !entry.ownsAllocation && entry.device != VK_NULL_HANDLE)
		{
			vkDestroyImage(entry.device, entry.image, nullptr);
		}
	}

	void ResourceRegistry::DestroyBufferEntryNow(const BufferEntry& entry, GpuMemoryTracker* memoryTracker)
	{
		if (entry.buffer == VK_NULL_HANDLE)
		{
			return;
		}
		if (memoryTracker != nullptr && entry.deviceAddress != 0)
		{
			memoryTracker->Unregister(entry.deviceAddress);
		}
		if (entry.ownsAllocation && entry.allocation != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(entry.allocator, entry.buffer, entry.allocation);
		}
		else if (!entry.ownsAllocation && entry.device != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(entry.device, entry.buffer, nullptr);
		}
	}

	void ResourceRegistry::DestroyPipelineEntryNow(const PipelineEntry& entry)
	{
		if (entry.device == VK_NULL_HANDLE)
		{
			return;
		}
		if (entry.vertexShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderEXT(entry.device, entry.vertexShader, nullptr);
		}
		if (entry.fragmentShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderEXT(entry.device, entry.fragmentShader, nullptr);
		}
		if (entry.computeShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderEXT(entry.device, entry.computeShader, nullptr);
		}
	}

} // namespace aether

namespace
{
	::aether::ResourceRegistry* s_reg = nullptr;
}

namespace aether::gpu
{
	void ResourceRegistry::Initialize(const ResourceRegistryInitDesc& desc) noexcept
	{
		s_reg = static_cast<::aether::ResourceRegistry*>(desc.backendRegistry);
		AE_ASSERT(s_reg != nullptr, "ResourceRegistry forwarding pointer is null.");
	}

	BufferHandle ResourceRegistry::CreateBuffer(const BufferDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateBuffer(d, loc);
	}

	BufferHandle ResourceRegistry::CreateMappedBuffer(const MappedBufferDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateMappedBuffer(d, loc);
	}

	TextureHandle ResourceRegistry::CreateTexture(const TextureDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateTexture(d, loc);
	}

	TextureHandle ResourceRegistry::CreateAliasedTexture(const TextureDesc& desc, void* existingAllocation, DeviceSize memoryOffset, const char* debugName) noexcept
	{
		return s_reg->CreateAliasedTexture(desc, static_cast<VmaAllocation>(existingAllocation), static_cast<VkDeviceSize>(memoryOffset), debugName ? std::string_view(debugName) : std::string_view{});
	}

	BufferHandle ResourceRegistry::CreateAliasedBuffer(DeviceSize size, BufferUsage usage, void* existingAllocation, DeviceSize memoryOffset, const char* debugName) noexcept
	{
		return s_reg->CreateAliasedBuffer(static_cast<VkDeviceSize>(size), gpu::ToVk(usage), static_cast<VmaAllocation>(existingAllocation), static_cast<VkDeviceSize>(memoryOffset), debugName ? std::string_view(debugName) : std::string_view{});
	}

	MappedBufferView ResourceRegistry::ResolveMappedBuffer(BufferHandle h) noexcept
	{
		return s_reg->ResolveMappedBuffer(h);
	}

	void ResourceRegistry::FlushMappedBuffer(BufferHandle h, DeviceSize o, DeviceSize s) noexcept
	{
		s_reg->FlushMappedBuffer(h, o, s);
	}

	void ResourceRegistry::InvalidateMappedBuffer(BufferHandle h, DeviceSize o, DeviceSize s) noexcept
	{
		s_reg->InvalidateMappedBuffer(h, o, s);
	}

	void ResourceRegistry::Destroy(BufferHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	void ResourceRegistry::Destroy(TextureHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	void ResourceRegistry::Destroy(PipelineHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	PipelineHandle ResourceRegistry::CreateComputePipeline(Device device, const ComputePipelineDesc& desc) noexcept
	{
		const vkutil::ComputePipelineDesc vkDesc{
		        .shaderVfsPath = desc.shaderVfsPath,
		        .shaderEntry = desc.shaderEntry,
		        .debugName = desc.debugName,
		        .descriptorHeapMappings = desc.descriptorHeapMappings,
		};
		const auto entryExp = vkutil::CreateComputePipelineEntry(device, vkDesc);
		if (!entryExp.has_value())
		{
			return PipelineHandle{};
		}
		return s_reg->RegisterPipeline(entryExp.value(), desc.debugName ? std::string_view(desc.debugName) : std::string_view{});
	}

	PipelineHandle ResourceRegistry::CreateGraphicsPipeline(Device device, const GraphicsPipelineDesc& desc) noexcept
	{
		const GraphicsPipeline::Desc vkDesc{
		        .shaderVfsPath = desc.shaderVfsPath,
		        .fragmentVfsPath = desc.fragmentVfsPath != nullptr ? std::string_view(desc.fragmentVfsPath) : std::string_view{},
		        .vertexEntry = desc.vertexEntry,
		        .fragmentEntry = desc.fragmentEntry,
		        .colorFormat = desc.colorFormat,
		        .depthFormat = desc.depthFormat,
		        .depthTestEnable = desc.depthTestEnable,
		        .depthWriteEnable = desc.depthWriteEnable,
		        .depthCompareOp = desc.depthCompareOp,
		        .blendEnable = desc.blendEnable,
		        .blendMode = desc.blendMode,
		        .topology = desc.topology,
		        .polygonMode = desc.polygonMode,
		        .cullMode = desc.cullMode,
		        .vertexBindings = desc.vertexBindings,
		        .vertexAttributes = desc.vertexAttributes,
		        .lineWidthDynamic = desc.lineWidthDynamic,
		        .descriptorHeapMappings = desc.descriptorHeapMappings,
		};
		const auto entryExp = vkutil::CreateGraphicsPipelineEntry(device, vkDesc);
		if (!entryExp.has_value())
		{
			return PipelineHandle{};
		}
		return s_reg->RegisterPipeline(entryExp.value(), desc.debugName ? std::string_view(desc.debugName) : std::string_view{});
	}

	ResourceRegistry::ResolvedPipeline ResourceRegistry::ResolvePipeline(PipelineHandle h) noexcept
	{
		const ::aether::ResourceRegistry::PipelineEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedPipeline out{};
		out.state = static_cast<const void*>(entry);
		return out;
	}

	ResourceRegistry::ResolvedTexture ResourceRegistry::ResolveTexture(TextureHandle h) noexcept
	{
		const ::aether::ResourceRegistry::TextureEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedTexture out{};
		out.view = static_cast<ImageView>(entry->view);
		out.format = gpu::FromVk(entry->format);
		out.extent = gpu::Extent2D{entry->extent.width, entry->extent.height};
		out.mipLevels = entry->mipLevels;
		out.arrayLayers = entry->arrayLayers;
		out.usage = static_cast<ImageUsage>(entry->usage);
		return out;
	}

	ResourceRegistry::ResolvedBuffer ResourceRegistry::ResolveBuffer(BufferHandle h) noexcept
	{
		const ::aether::ResourceRegistry::BufferEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedBuffer out{};
		out.deviceAddress = entry->deviceAddress;
		out.size = entry->size;
		out.usage = static_cast<BufferUsage>(entry->usage);
		return out;
	}

	void* ResourceRegistry::ResolveBufferVkHandle(BufferHandle handle) noexcept
	{
		const ::aether::ResourceRegistry::BufferEntry* entry = s_reg->Resolve(handle);
		if (entry == nullptr)
		{
			return nullptr;
		}
		return static_cast<void*>(entry->buffer);
	}

	gpu::Image ResourceRegistry::ResolveTextureImage(TextureHandle handle) noexcept
	{
		if (s_reg == nullptr)
		{
			return nullptr;
		}
		const ::aether::ResourceRegistry::TextureEntry* entry = s_reg->Resolve(handle);
		if (entry == nullptr)
		{
			return nullptr;
		}
		return static_cast<gpu::Image>(entry->image);
	}

	void ResourceRegistry::SetBufferName(BufferHandle handle, const char* name)
	{
		s_reg->SetBufferName(handle, name);
	}

	void ResourceRegistry::SetTextureName(TextureHandle handle, const char* name)
	{
		s_reg->SetTextureName(handle, name);
	}

	void ResourceRegistry::SetBindlessManager(aether::BindlessManager* mgr)
	{
		s_reg->SetBindlessManager(mgr);
	}

	void ResourceRegistry::EnsureBindlessSampled(TextureHandle handle, ImageAspect aspectMask, ImageLayout descriptorLayout)
	{
		auto result = s_reg->EnsureBindlessSampled(handle, aspectMask, descriptorLayout);
		if (!result)
		{
			AE_WARN(LogCategory::Vulkan, "ResourceRegistry::EnsureBindlessSampled failed: {}", result.error());
		}
	}

	bool ResourceRegistry::HasBindlessSampled(TextureHandle handle)
	{
		return s_reg->HasBindlessSampled(handle);
	}

	std::uint32_t ResourceRegistry::GetBindlessSampledSlot(TextureHandle handle)
	{
		return s_reg->GetBindlessSampledSlot(handle);
	}

	Format ResourceRegistry::GetTextureFormat(TextureHandle handle)
	{
		return s_reg->GetTextureFormat(handle);
	}

	Extent2D ResourceRegistry::GetTextureExtent(TextureHandle handle)
	{
		return s_reg->GetTextureExtent(handle);
	}

	std::uint32_t ResourceRegistry::GetTextureMipLevels(TextureHandle handle)
	{
		return s_reg->GetTextureMipLevels(handle);
	}

	std::uint32_t ResourceRegistry::GetTextureArrayLayers(TextureHandle handle)
	{
		return s_reg->GetTextureArrayLayers(handle);
	}

	ImageUsage ResourceRegistry::GetTextureUsage(TextureHandle handle)
	{
		return s_reg->GetTextureUsage(handle);
	}

	std::vector<DebugTextureInfo> ResourceRegistry::ListDebugTextures()
	{
		return s_reg->ListDebugTextures();
	}

	DeviceSize ResourceRegistry::GetBufferSize(BufferHandle handle)
	{
		return s_reg->GetBufferSize(handle);
	}

	BufferUsage ResourceRegistry::GetBufferUsage(BufferHandle handle)
	{
		return s_reg->GetBufferUsage(handle);
	}

	const void* ResourceRegistry::GetViewCreateInfo(TextureHandle handle) noexcept
	{
		return s_reg->GetViewCreateInfo(handle);
	}
} // namespace aether::gpu
