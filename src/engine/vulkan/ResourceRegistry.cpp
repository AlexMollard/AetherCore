#include "vulkan/ResourceRegistry.hpp"

#include <algorithm>

#include "utils/Assert.hpp"
#include "utils/Backtrace.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "gpu/BindlessManager.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GpuTypesVk.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	namespace
	{
		inline constexpr std::uint32_t kIndexInvalid = 0x0000FFFFu;
		inline constexpr std::uint32_t kGenerationInvalid = 0u;
		inline constexpr std::uint32_t kGenerationWrap = 65536u;

#ifndef NDEBUG
		// Build a multi-line frame list from the slot's ring buffer.
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
#endif // !NDEBUG

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

		// Deduces the canonical VkImageAspectFlags for a given format.
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
		if (m_shutdown)
		{
			return;
		}
		m_shutdown = true;

		// Drain pending destruction first, then warn-and-tear-down any slots still holding entries.
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
				DestroyBufferEntryNow(*slot.entry);
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
		m_device = device;
		m_allocator = allocator;
		m_textures.reserve(1024);
		m_buffers.reserve(1024);
		m_pipelines.reserve(256);
	}

	gpu::BufferHandle ResourceRegistry::CreateBuffer(const gpu::BufferDesc& desc, std::source_location loc) noexcept
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = desc.size,
		        .usage = gpu::ToVkBufferUsage(desc.usage),
		};

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

		const VkBufferUsageFlags vkUsage = gpu::ToVkBufferUsage(desc.usage);

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = desc.size,
		        .usage = vkUsage,
		};

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
		        .extent = {desc.extent.width, desc.extent.height, 1u},
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
		        .subresourceRange = {gpu::ToVk(desc.aspect), 0, desc.mipLevels, 0, desc.arrayLayers},
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
		entry.extent = {desc.extent.width, desc.extent.height};
		entry.usage = imageInfo.usage;
		entry.aspect = gpu::ToVk(desc.aspect);
		entry.device = m_device;
		entry.allocation = allocation;
		entry.allocator = m_allocator;
		entry.ownsAllocation = true;
		entry.mipLevels = desc.mipLevels;
		entry.arrayLayers = desc.arrayLayers;

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

		const auto* entry = Resolve(handle);
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

		const auto* entry = Resolve(handle);
		if (!entry || entry->allocation == VK_NULL_HANDLE)
		{
			return;
		}

		vmaFlushAllocation(m_allocator, entry->allocation, offset, size);
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

	gpu::BufferHandle ResourceRegistry::CreateAliasedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName)
	{
		AE_ASSERT(m_device != VK_NULL_HANDLE, "ResourceRegistry not initialized");
		AE_ASSERT(m_allocator != VK_NULL_HANDLE, "ResourceRegistry not initialized");

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = size,
		        .usage = usage,
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

		if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
		{
			const VkBufferDeviceAddressInfo addrInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = buffer,
			};
			entry.deviceAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
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
		        .extent = {desc.extent.width, desc.extent.height, 1u},
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
		        .subresourceRange = {aspect, 0, desc.mipLevels, 0, desc.arrayLayers},
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
		entry.extent = {desc.extent.width, desc.extent.height};
		entry.usage = imageInfo.usage;
		entry.aspect = aspect;
		entry.device = m_device;
		entry.allocation = existingAllocation;
		entry.allocator = m_allocator;
		entry.ownsAllocation = false;
		entry.mipLevels = desc.mipLevels;
		entry.arrayLayers = desc.arrayLayers;

		return RegisterTexture(entry, debugName.empty() ? std::string_view{} : debugName);
	}

	Expected<void> ResourceRegistry::EnsureBindlessSampled(gpu::TextureHandle handle, const gpu::ImageAspect aspectMask, const gpu::ImageLayout descriptorLayout, const TextureFilter filter, const gpu::SamplerAddressMode addressMode)
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

		// Use existing view or create one
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
		}

		const gpu::Filter gpuFilter = (filter == TextureFilter::Nearest) ? gpu::Filter::Nearest : gpu::Filter::Linear;
		const gpu::SamplerMipmapMode gpuMipmapMode = (filter == TextureFilter::Nearest) ? gpu::SamplerMipmapMode::Nearest : gpu::SamplerMipmapMode::Linear;

		Expected<gpu::Sampler> samplerResult = m_bindlessManager->GetOrCreateSampler(gpuFilter, gpuMipmapMode, addressMode);
		if (!samplerResult)
		{
			if (makeView)
			{
				vkDestroyImageView(entry->device, view, nullptr);
			}
			AE_UNEXPECTED(samplerResult.error());
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

		// Pass the engine-side layout directly to UpdateSampledImage (no Vk round-trip).
		Expected<void> updateResult = m_bindlessManager->UpdateSampledImage(*slotResult, static_cast<gpu::ImageView>(view), gpu::Sampler(*samplerResult), descriptorLayout);
		if (!updateResult)
		{
			m_bindlessManager->FreeSampledImageSlot(*slotResult);
			if (makeView)
			{
				vkDestroyImageView(entry->device, view, nullptr);
			}
			AE_UNEXPECTED(updateResult.error());
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
			const std::uint32_t i = static_cast<std::uint32_t>(m_textures.size());
			m_textures.push_back(TextureSlot{});
			return i;
		}
		// Exhausted - caller will see an invalid handle and AE_ASSERT.
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
			const std::uint32_t i = static_cast<std::uint32_t>(m_buffers.size());
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
			const std::uint32_t i = static_cast<std::uint32_t>(m_pipelines.size());
			m_pipelines.push_back(PipelineSlot{});
			return i;
		}
		AE_ASSERT(false, "ResourceRegistry: out of PipelineSlot indices (16-bit index space exhausted).");
		return kIndexInvalid;
	}

	gpu::TextureHandle ResourceRegistry::RegisterTexture(const TextureEntry& entry, std::string_view debugName, [[maybe_unused]] std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterTexture called after Shutdown.");
		const std::uint32_t idx = AcquireTextureSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_liveTextureCount;
		m_textures[idx].entry = entry;
		m_textures[idx].debugName = debugName.empty() ? std::to_string(idx) : std::string(debugName);
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

	gpu::BufferHandle ResourceRegistry::RegisterBuffer(const BufferEntry& entry, std::string_view debugName, [[maybe_unused]] std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterBuffer called after Shutdown.");
		const std::uint32_t idx = AcquireBufferSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_liveBufferCount;
		m_buffers[idx].entry = entry;
		m_buffers[idx].debugName = debugName.empty() ? std::to_string(idx) : std::string(debugName);
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

	gpu::PipelineHandle ResourceRegistry::RegisterPipeline(const PipelineEntry& entry, std::string_view debugName, [[maybe_unused]] std::source_location loc)
	{
		AE_ASSERT(!m_shutdown, "ResourceRegistry::RegisterPipeline called after Shutdown.");
		const std::uint32_t idx = AcquirePipelineSlot();
		if (idx == kIndexInvalid)
		{
			return {};
		}
		++m_livePipelineCount;
		m_pipelines[idx].entry = entry;
		m_pipelines[idx].debugName = debugName.empty() ? std::to_string(idx) : std::string(debugName);
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
		// The handle is invalidated for CPU resolve immediately (slot is
		// reset and the generation is bumped below). The actual GPU/VkImage
		// destruction is deferred to kMaxFramesInFlight frames from now via
		// the queued destroyer; the WaitIdle at the matching frame boundary
		// guarantees the GPU is no longer reading from this texture.
		const TextureEntry entry = *slot.entry;
		--m_liveTextureCount;
		slot.entry.reset();
		// Bump generation on reuse so subsequent handles to this slot fail IsValid().
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
		        .fn = [this, entry]() { DestroyBufferEntryNow(entry); },
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
		// The "next" frame becomes the current. The ring slot we are about
		// to retire (m_currentFrame after the increment) holds destroyers
		// queued kMaxFramesInFlight frames ago, when the GPU was given the
		// corresponding submission. By the time we reach it, the engine
		// has called vkDeviceWaitIdle (or the fence/semaphore for that
		// frame has signalled), so destruction is safe.
		const std::uint32_t nextFrame = (m_currentFrame + 1u) % kMaxFramesInFlight;
		RunDestroyersInRing(m_pendingDestructions[nextFrame]);
		m_currentFrame = nextFrame;
	}

	void ResourceRegistry::DrainAll()
	{
		// After Shutdown we are guaranteed the GPU is idle (GpuDevice::
		// Shutdown calls m_gfx->Shutdown() which itself calls
		// vkDeviceWaitIdle), so running *all* queued destroyers is safe.
		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			RunDestroyersInRing(m_pendingDestructions[i]);
		}
	}

	void ResourceRegistry::RunDestroyersInRing(std::vector<PendingDestruction>& ring)
	{
		// Move out first so a destructor that chains Destroy() into the registry does not invalidate iteration.
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

	void ResourceRegistry::DestroyBufferEntryNow(const BufferEntry& entry)
	{
		if (entry.buffer == VK_NULL_HANDLE)
		{
			return;
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
		// Linked pipeline first - the libraries it was built from can go
		// immediately after (the linked pipeline retained its own state copy).
		if (entry.pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(entry.device, entry.pipeline, nullptr);
		}
		if (entry.vertInputLib != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(entry.device, entry.vertInputLib, nullptr);
		}
		if (entry.preRasterLib != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(entry.device, entry.preRasterLib, nullptr);
		}
		if (entry.fragShaderLib != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(entry.device, entry.fragShaderLib, nullptr);
		}
		if (entry.fragOutputLib != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(entry.device, entry.fragOutputLib, nullptr);
		}
		if (entry.layout != VK_NULL_HANDLE && entry.ownsLayout)
		{
			vkDestroyPipelineLayout(entry.device, entry.layout, nullptr);
		}
	}

	std::uint32_t ResourceRegistry::LiveTextureCount() const
	{
		return m_liveTextureCount;
	}

	std::uint32_t ResourceRegistry::LiveBufferCount() const
	{
		return m_liveBufferCount;
	}

	std::uint32_t ResourceRegistry::LivePipelineCount() const
	{
		return m_livePipelineCount;
	}
} // namespace aether
