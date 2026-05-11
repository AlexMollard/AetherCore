#include "vulkan/UniqueImage.hpp"

#include <format>
#include <utility>

#include "utils/AetherExceptions.hpp"
#include "material/BindlessManager.hpp"

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
		}
	} // anonymous namespace

	UniqueImage UniqueImage::Create(VkDevice device, VmaAllocator allocator, const Desc& desc)
	{
		const VkImageCreateInfo imageInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = desc.format,
			.extent = { desc.extent.width, desc.extent.height, 1u },
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
		UniqueImage out = Create(allocator, imageInfo, allocInfo);

		// Create the default view so callers can use GetDefaultView() immediately
		// without a separate vkCreateImageView call. The device is stored so
		// Reset() (via ReleaseBindlessSampled) can destroy the view.
		const VkImageAspectFlags aspect = DeduceAspect(desc.format);
		const VkImageViewCreateInfo viewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = out.m_image,
			.viewType = desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
			.format = desc.format,
			.subresourceRange = { aspect, 0, desc.mipLevels, 0, desc.arrayLayers },
		};
		const VkResult viewResult = vkCreateImageView(device, &viewInfo, nullptr, &out.m_defaultView);
		if (viewResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("UniqueImage::Create: failed to create default view. VkResult={}", static_cast<int>(viewResult)));
		}
		out.m_bindlessDevice = device; // allows Reset() to destroy the view
		return out;
	}

	UniqueImage UniqueImage::Create(VmaAllocator allocator, const VkImageCreateInfo& imageCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo)
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
			throw VulkanError(std::format("Failed to create VMA image. VkResult={}", static_cast<int>(createResult)));
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

	void UniqueImage::EnsureBindlessSampled(BindlessManager& bindlessManager, const VkDevice device, const VkImageAspectFlags aspectMask, const VkImageLayout descriptorLayout, const TextureFilter filter)
	{
		if (!(*this))
		{
			throw VulkanError("Cannot bindless-register an invalid image handle.");
		}

		if (device == VK_NULL_HANDLE)
		{
			throw VulkanError("Cannot bindless-register image: VkDevice is null.");
		}

		if (m_bindlessSlot != kInvalidBindlessSlot)
		{
			return;
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
        .viewType = m_arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                      : VK_IMAGE_VIEW_TYPE_2D,
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
				throw VulkanError(std::format("Failed to create image view for bindless registration. VkResult={}", static_cast<int>(viewResult)));
			}
		}

		const VkSamplerCreateInfo samplerCreateInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.magFilter = filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
			.minFilter = filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
			.mipmapMode = filter == TextureFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias = 0.0f,
			.anisotropyEnable = VK_FALSE,
			.maxAnisotropy = 1.0f,
			.compareEnable = VK_FALSE,
			.compareOp = VK_COMPARE_OP_ALWAYS,
			.minLod = 0.0f,
			.maxLod = static_cast<float>(m_mipLevels),
			.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
		};

		VkSampler sampler = VK_NULL_HANDLE;
		const VkResult samplerResult = vkCreateSampler(device, &samplerCreateInfo, nullptr, &sampler);
		if (samplerResult != VK_SUCCESS)
		{
			if (ownView)
			{
				vkDestroyImageView(device, view, nullptr);
			}
			throw VulkanError(std::format("Failed to create sampler for bindless registration. VkResult={}", static_cast<int>(samplerResult)));
		}

		std::uint32_t slot = kInvalidBindlessSlot;
		try
		{
			slot = bindlessManager.AllocateSampledImageSlot();
			bindlessManager.UpdateSampledImage(slot, view, sampler, descriptorLayout);
		}
		catch (...)
		{
			if (slot != kInvalidBindlessSlot)
			{
				bindlessManager.FreeSampledImageSlot(slot);
			}
			vkDestroySampler(device, sampler, nullptr);
			if (ownView)
			{
				vkDestroyImageView(device, view, nullptr);
			}
			throw;
		}

		m_bindlessManager = &bindlessManager;
		m_bindlessDevice = device;
		m_defaultView = view;
		m_defaultSampler = sampler;
		m_bindlessSlot = slot;
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

		if (m_defaultSampler != VK_NULL_HANDLE && m_bindlessDevice != VK_NULL_HANDLE)
		{
			vkDestroySampler(m_bindlessDevice, m_defaultSampler, nullptr);
		}

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
} // namespace aether
