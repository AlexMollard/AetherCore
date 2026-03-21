#include "UniqueImage.hpp"

#include <format>
#include <utility>

#include "BindlessManager.hpp"
#include "MeowExceptions.hpp"

namespace meow
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

	UniqueImage UniqueImage::Create(
		VmaAllocator allocator,
		const VkImageCreateInfo& imageCreateInfo,
		const VmaAllocationCreateInfo& allocationCreateInfo)
	{
		UniqueImage out;
		out.m_allocator = allocator;
		out.m_extent = imageCreateInfo.extent;
		out.m_format = imageCreateInfo.format;
		out.m_usage = imageCreateInfo.usage;
		out.m_mipLevels = imageCreateInfo.mipLevels;
		out.m_arrayLayers = imageCreateInfo.arrayLayers;
		out.m_lastKnownLayout = imageCreateInfo.initialLayout;

		const VkResult createResult = vmaCreateImage(
			allocator,
			&imageCreateInfo,
			&allocationCreateInfo,
			&out.m_image,
			&out.m_allocation,
			&out.m_allocationInfo);

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

	void UniqueImage::EnsureBindlessSampled(
		BindlessManager& bindlessManager,
		const VkDevice device,
		const VkImageAspectFlags aspectMask,
		const VkImageLayout descriptorLayout)
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

		const VkImageViewCreateInfo viewCreateInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.image = m_image,
			.viewType = m_arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
			.format = m_format,
			.components = {
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = VK_COMPONENT_SWIZZLE_IDENTITY,
			},
			.subresourceRange = {
				.aspectMask = aspectMask,
				.baseMipLevel = 0,
				.levelCount = m_mipLevels,
				.baseArrayLayer = 0,
				.layerCount = m_arrayLayers,
			},
		};

		VkImageView view = VK_NULL_HANDLE;
		const VkResult viewResult = vkCreateImageView(device, &viewCreateInfo, nullptr, &view);
		if (viewResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create image view for bindless registration. VkResult={}", static_cast<int>(viewResult)));
		}

		const VkSamplerCreateInfo samplerCreateInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
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
			vkDestroyImageView(device, view, nullptr);
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
			vkDestroyImageView(device, view, nullptr);
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
}
