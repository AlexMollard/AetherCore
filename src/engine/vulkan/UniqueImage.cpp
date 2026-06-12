#include "vulkan/UniqueImage.hpp"

#include <format>
#include <utility>

#include "utils/Expected.hpp"
#include "gpu/BindlessManager.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/GpuEnumConversions.hpp"

namespace aether
{
	UniqueImage::~UniqueImage()
	{
		Reset();
	}

	UniqueImage::UniqueImage(UniqueImage&& other) noexcept
	      : m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE)),
	        m_image(std::exchange(other.m_image, VK_NULL_HANDLE)),
	        m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE)),
	        m_allocationInfo(other.m_allocationInfo),
	        m_extent(other.m_extent),
	        m_format(other.m_format),
	        m_usage(other.m_usage),
	        m_mipLevels(other.m_mipLevels),
	        m_arrayLayers(other.m_arrayLayers),
	        m_lastKnownLayout(other.m_lastKnownLayout),
	        m_queueFamilyOwner(other.m_queueFamilyOwner),
	        m_virtualResourceId(other.m_virtualResourceId),
	        m_bindlessManager(std::exchange(other.m_bindlessManager, nullptr)),
	        m_bindlessDevice(std::exchange(other.m_bindlessDevice, VK_NULL_HANDLE)),
	        m_defaultView(std::exchange(other.m_defaultView, VK_NULL_HANDLE)),
	        m_defaultSampler(std::exchange(other.m_defaultSampler, VK_NULL_HANDLE)),
	        m_bindlessSlot(std::exchange(other.m_bindlessSlot, kInvalidBindlessSlot))
	{
		other.m_allocationInfo = {};
		other.m_extent = {};
		other.m_format = VK_FORMAT_UNDEFINED;
		other.m_usage = 0;
		other.m_mipLevels = 1;
		other.m_arrayLayers = 1;
		other.m_lastKnownLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		other.m_queueFamilyOwner = VK_QUEUE_FAMILY_IGNORED;
		other.m_virtualResourceId = 0;
	}

	UniqueImage& UniqueImage::operator=(UniqueImage&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		Reset();

		m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
		m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
		m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
		m_allocationInfo = other.m_allocationInfo;
		m_extent = other.m_extent;
		m_format = other.m_format;
		m_usage = other.m_usage;
		m_mipLevels = other.m_mipLevels;
		m_arrayLayers = other.m_arrayLayers;
		m_lastKnownLayout = other.m_lastKnownLayout;
		m_queueFamilyOwner = other.m_queueFamilyOwner;
		m_virtualResourceId = other.m_virtualResourceId;
		m_bindlessManager = std::exchange(other.m_bindlessManager, nullptr);
		m_bindlessDevice = std::exchange(other.m_bindlessDevice, VK_NULL_HANDLE);
		m_defaultView = std::exchange(other.m_defaultView, VK_NULL_HANDLE);
		m_defaultSampler = std::exchange(other.m_defaultSampler, VK_NULL_HANDLE);
		m_bindlessSlot = std::exchange(other.m_bindlessSlot, kInvalidBindlessSlot);

		other.m_allocationInfo = {};
		other.m_extent = {};
		other.m_format = VK_FORMAT_UNDEFINED;
		other.m_usage = 0;
		other.m_mipLevels = 1;
		other.m_arrayLayers = 1;
		other.m_lastKnownLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		other.m_queueFamilyOwner = VK_QUEUE_FAMILY_IGNORED;
		other.m_virtualResourceId = 0;

		return *this;
	}

	namespace
	{
		// Deduces the canonical VkImageAspectFlags for a given format.
		// Depth/stencil combinations set both bits; everything else is COLOR.
		VkImageAspectFlags DeduceAspect(VkFormat format)
		{
#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wswitch-enum"
#endif
			switch (format)
			{
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
	} // anonymous namespace

	Expected<UniqueImage> UniqueImage::Create(VkDevice device, VmaAllocator allocator, const Desc& desc)
	{
		const VkImageCreateInfo imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = gpu::ToVk(desc.format),
		        .extent = {desc.extent.width, desc.extent.height, 1u},
		        .mipLevels = desc.mipLevels,
		        .arrayLayers = desc.arrayLayers,
		        .samples = desc.samples,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = desc.usage,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		const VmaAllocationCreateInfo allocInfo{
		        .usage = desc.memoryUsage,
		};
		Expected<UniqueImage> out = Create(allocator, imageInfo, allocInfo);
		if (!out)
		{
			AE_UNEXPECTED(out.error());
		}

		// Create the default view so callers can use GetDefaultView() immediately
		// without a separate vkCreateImageView call. The device is stored so
		// Reset() (via ReleaseBindlessSampled) can destroy the view.
		const VkImageAspectFlags aspect = DeduceAspect(gpu::ToVk(desc.format));
		const VkImageViewCreateInfo viewInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		        .image = out->m_image,
		        .viewType = desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		        .format = gpu::ToVk(desc.format),
		        .subresourceRange = {aspect, 0, desc.mipLevels, 0, desc.arrayLayers},
		};
		const VkResult viewResult = vkCreateImageView(device, &viewInfo, nullptr, &out->m_defaultView);
		if (viewResult != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(viewResult), "UniqueImage::Create: failed to create default view"));
		}
		out->m_bindlessDevice = device; // allows Reset() to destroy the view
		if (desc.debugName)
		{
			out->SetName(device, desc.debugName);
		}
		return out;
	}

	Expected<UniqueImage> UniqueImage::Create(VmaAllocator allocator, const VkImageCreateInfo& imageCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo)
	{
		UniqueImage out;
		out.m_allocator = allocator;
		out.m_extent = imageCreateInfo.extent;
		out.m_format = imageCreateInfo.format;
		out.m_usage = imageCreateInfo.usage;
		out.m_mipLevels = imageCreateInfo.mipLevels;
		out.m_arrayLayers = imageCreateInfo.arrayLayers;
		out.m_lastKnownLayout = imageCreateInfo.initialLayout;

		const VkResult createResult = vmaCreateImage(allocator, &imageCreateInfo, &allocationCreateInfo, &out.m_image, &out.m_allocation, &out.m_allocationInfo);

		if (createResult != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(createResult), "Failed to create VMA image"));
		}

		return out;
	}

	void UniqueImage::Reset()
	{
		ReleaseBindlessSampled();

		if (m_image != VK_NULL_HANDLE && m_allocation != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE)
		{
			vmaDestroyImage(m_allocator, m_image, m_allocation);
		}

		m_image = VK_NULL_HANDLE;
		m_allocation = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
		m_allocationInfo = {};
		m_extent = {};
		m_format = VK_FORMAT_UNDEFINED;
		m_usage = 0;
		m_mipLevels = 1;
		m_arrayLayers = 1;
		m_lastKnownLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		m_queueFamilyOwner = VK_QUEUE_FAMILY_IGNORED;
		m_virtualResourceId = 0;
	}

	Expected<void> UniqueImage::EnsureBindlessSampled(BindlessManager& bindlessManager, const VkDevice device, const VkImageAspectFlags aspectMask, const VkImageLayout descriptorLayout, const TextureFilter filter)
	{
		if (!(*this))
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "Cannot bindless-register an invalid image handle"));
		}

		if (device == VK_NULL_HANDLE)
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "Cannot bindless-register image: VkDevice is null"));
		}

		if (m_bindlessSlot != kInvalidBindlessSlot)
		{
			return {};
		}

		// Reuse the view created by the high-level Create() overload if available,
		// rather than creating a redundant second view on the same image.
		const bool ownView = (m_defaultView == VK_NULL_HANDLE);
		VkImageView view = m_defaultView;

		if (ownView)
		{
			const VkImageViewCreateInfo viewCreateInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			        .pNext = nullptr,
			        .flags = 0,
			        .image = m_image,
			        .viewType = m_arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
			        .format = m_format,
			        .components =
			                {
			                        .r = VK_COMPONENT_SWIZZLE_IDENTITY,
			                        .g = VK_COMPONENT_SWIZZLE_IDENTITY,
			                        .b = VK_COMPONENT_SWIZZLE_IDENTITY,
			                        .a = VK_COMPONENT_SWIZZLE_IDENTITY,
			                },
			        .subresourceRange =
			                {
			                        .aspectMask = aspectMask,
			                        .baseMipLevel = 0,
			                        .levelCount = m_mipLevels,
			                        .baseArrayLayer = 0,
			                        .layerCount = m_arrayLayers,
			                },
			};
			const VkResult viewResult = vkCreateImageView(device, &viewCreateInfo, nullptr, &view);
			if (viewResult != VK_SUCCESS)
			{
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(viewResult), "Failed to create image view for bindless registration"));
			}
		}

		const gpu::Filter gpuFilter = (filter == TextureFilter::Nearest) ? gpu::Filter::Nearest : gpu::Filter::Linear;
		const gpu::SamplerMipmapMode gpuMipmapMode = (filter == TextureFilter::Nearest) ? gpu::SamplerMipmapMode::Nearest : gpu::SamplerMipmapMode::Linear;

		Expected<gpu::Sampler> samplerResult = bindlessManager.GetOrCreateSampler(gpuFilter, gpuMipmapMode, gpu::SamplerAddressMode::Repeat);
		if (!samplerResult)
		{
			if (ownView)
			{
				vkDestroyImageView(device, view, nullptr);
			}
			return Unexpected{AetherError::Vulkan(0, "Failed to get cached sampler for bindless registration")};
		}
		gpu::Sampler sampler = *samplerResult;

		// Allocate bindless slot - if this fails, clean up view.
		Expected<std::uint32_t> slotResult = bindlessManager.AllocateSampledImageSlot();
		if (!slotResult)
		{
			if (ownView)
			{
				vkDestroyImageView(device, view, nullptr);
			}
			AE_UNEXPECTED(slotResult.error());
		}

		// Update descriptor - if this fails, free the slot and clean up view.
		Expected<void> updateResult = bindlessManager.UpdateSampledImage(*slotResult, static_cast<gpu::ImageView>(view), sampler, gpu::FromVk(descriptorLayout));
		if (!updateResult)
		{
			bindlessManager.FreeSampledImageSlot(*slotResult);
			if (ownView)
			{
				vkDestroyImageView(device, view, nullptr);
			}
			AE_UNEXPECTED(updateResult.error());
		}

		m_bindlessManager = &bindlessManager;
		m_bindlessDevice = device;
		m_defaultView = view;
		m_defaultSampler = static_cast<VkSampler>(sampler);
		m_bindlessSlot = *slotResult;
		return {};
	}

	void UniqueImage::ReleaseBindlessSampled(const bool deferSlotFree)
	{
		if (m_bindlessManager != nullptr && m_bindlessSlot != kInvalidBindlessSlot)
		{
			if (deferSlotFree)
			{
				m_bindlessManager->FreeSampledImageSlotDeferred(m_bindlessSlot);
			}
			else
			{
				m_bindlessManager->FreeSampledImageSlot(m_bindlessSlot);
			}
		}

		// Sampler is cached in BindlessManager - do not destroy.
		if (m_defaultView != VK_NULL_HANDLE && m_bindlessDevice != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_bindlessDevice, m_defaultView, nullptr);
		}

		m_bindlessManager = nullptr;
		m_bindlessDevice = VK_NULL_HANDLE;
		m_defaultView = VK_NULL_HANDLE;
		m_defaultSampler = VK_NULL_HANDLE;
		m_bindlessSlot = kInvalidBindlessSlot;
	}

	VkImage UniqueImage::Get() const
	{
		return m_image;
	}

	VmaAllocation UniqueImage::GetAllocation() const
	{
		return m_allocation;
	}

	VmaAllocator UniqueImage::GetAllocator() const
	{
		return m_allocator;
	}

	const VmaAllocationInfo& UniqueImage::GetAllocationInfo() const
	{
		return m_allocationInfo;
	}

	VkExtent3D UniqueImage::GetExtent() const
	{
		return m_extent;
	}

	VkFormat UniqueImage::GetFormat() const
	{
		return m_format;
	}

	VkImageUsageFlags UniqueImage::GetUsage() const
	{
		return m_usage;
	}

	std::uint32_t UniqueImage::GetMipLevels() const
	{
		return m_mipLevels;
	}

	std::uint32_t UniqueImage::GetArrayLayers() const
	{
		return m_arrayLayers;
	}

	VkImageLayout UniqueImage::GetLastKnownLayout() const
	{
		return m_lastKnownLayout;
	}

	std::uint32_t UniqueImage::GetQueueFamilyOwner() const
	{
		return m_queueFamilyOwner;
	}

	std::uint64_t UniqueImage::GetVirtualResourceId() const
	{
		return m_virtualResourceId;
	}

	bool UniqueImage::HasBindlessSampled() const
	{
		return m_bindlessSlot != kInvalidBindlessSlot;
	}

	std::uint32_t UniqueImage::GetBindlessSampledSlot() const
	{
		return m_bindlessSlot;
	}

	VkImageView UniqueImage::GetDefaultView() const
	{
		return m_defaultView;
	}

	VkSampler UniqueImage::GetDefaultSampler() const
	{
		return m_defaultSampler;
	}

	void UniqueImage::SetLastKnownLayout(const VkImageLayout layout)
	{
		m_lastKnownLayout = layout;
	}

	void UniqueImage::SetQueueFamilyOwner(const std::uint32_t queueFamilyIndex)
	{
		m_queueFamilyOwner = queueFamilyIndex;
	}

	void UniqueImage::SetVirtualResourceId(const std::uint64_t virtualResourceId)
	{
		m_virtualResourceId = virtualResourceId;
	}

	UniqueImage::operator bool() const
	{
		return m_image != VK_NULL_HANDLE;
	}

	void UniqueImage::SetName(VkDevice device, const char* name) const
	{
		if (m_image == VK_NULL_HANDLE || name == nullptr)
		{
			return;
		}
		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_image), VK_OBJECT_TYPE_IMAGE, name);
		if (m_defaultView != VK_NULL_HANDLE)
		{
			const std::string viewName = std::string(name) + ".View";
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_defaultView), VK_OBJECT_TYPE_IMAGE_VIEW, viewName.c_str());
		}
	}
} // namespace aether
