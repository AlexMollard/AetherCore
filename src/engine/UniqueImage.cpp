#include "UniqueImage.hpp"

#include <format>
#include <utility>

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
		m_virtualResourceId(other.m_virtualResourceId)
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
