#pragma once

#include <cstdint>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace meow
{
	class UniqueImage
	{
	public:
		UniqueImage() = default;
		~UniqueImage();

		UniqueImage(const UniqueImage&) = delete;
		UniqueImage& operator=(const UniqueImage&) = delete;

		UniqueImage(UniqueImage&& other) noexcept;
		UniqueImage& operator=(UniqueImage&& other) noexcept;

		static UniqueImage Create(
			VmaAllocator allocator,
			const VkImageCreateInfo& imageCreateInfo,
			const VmaAllocationCreateInfo& allocationCreateInfo);

		void Reset();

		[[nodiscard]] VkImage Get() const;
		[[nodiscard]] VmaAllocation GetAllocation() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] const VmaAllocationInfo& GetAllocationInfo() const;
		[[nodiscard]] VkExtent3D GetExtent() const;
		[[nodiscard]] VkFormat GetFormat() const;
		[[nodiscard]] VkImageUsageFlags GetUsage() const;
		[[nodiscard]] std::uint32_t GetMipLevels() const;
		[[nodiscard]] std::uint32_t GetArrayLayers() const;
		[[nodiscard]] VkImageLayout GetLastKnownLayout() const;
		[[nodiscard]] std::uint32_t GetQueueFamilyOwner() const;
		[[nodiscard]] std::uint64_t GetVirtualResourceId() const;

		void SetLastKnownLayout(VkImageLayout layout);
		void SetQueueFamilyOwner(std::uint32_t queueFamilyIndex);
		void SetVirtualResourceId(std::uint64_t virtualResourceId);

		explicit operator bool() const;

	private:
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkImage m_image = VK_NULL_HANDLE;
		VmaAllocation m_allocation = VK_NULL_HANDLE;
		VmaAllocationInfo m_allocationInfo{};
		VkExtent3D m_extent{};
		VkFormat m_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags m_usage = 0;
		std::uint32_t m_mipLevels = 1;
		std::uint32_t m_arrayLayers = 1;
		VkImageLayout m_lastKnownLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		std::uint32_t m_queueFamilyOwner = VK_QUEUE_FAMILY_IGNORED;
		std::uint64_t m_virtualResourceId = 0;
	};
}
