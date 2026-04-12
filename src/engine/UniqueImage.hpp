#pragma once

#include <cstdint>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace meow
{
	class BindlessManager;

	class UniqueImage
	{
	public:
		// Concise description for the common-case Create overload.
		// Aspect and sampler are deduced automatically from the format and usage.
		struct Desc
		{
			VkExtent2D            extent = {};
			VkFormat              format = VK_FORMAT_UNDEFINED;
			VkImageUsageFlags     usage = 0;
			std::uint32_t         mipLevels = 1;
			std::uint32_t         arrayLayers = 1;
			VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
			VmaMemoryUsage        memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		};

		UniqueImage() = default;
		~UniqueImage();

		UniqueImage(const UniqueImage&) = delete;
		UniqueImage& operator=(const UniqueImage&) = delete;

		UniqueImage(UniqueImage&& other) noexcept;
		UniqueImage& operator=(UniqueImage&& other) noexcept;

		// High-level overload: builds VkImageCreateInfo from Desc, allocates via VMA,
		// and creates the default VkImageView automatically.  Aspect is deduced from
		// the format; device is stored so Reset() can destroy the view.
		static UniqueImage Create(
			VkDevice device,
			VmaAllocator allocator,
			const Desc& desc);

		// Low-level overload: caller supplies the full Vulkan structs directly.
		// No default view is created; callers manage their own VkImageViews.
		static UniqueImage Create(
			VmaAllocator allocator,
			const VkImageCreateInfo& imageCreateInfo,
			const VmaAllocationCreateInfo& allocationCreateInfo);

		void Reset();
		void EnsureBindlessSampled(
			BindlessManager& bindlessManager,
			VkDevice device,
			VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			VkImageLayout descriptorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		void ReleaseBindlessSampled(bool deferSlotFree = true);

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
		[[nodiscard]] bool HasBindlessSampled() const;
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot() const;
		[[nodiscard]] VkImageView GetDefaultView() const;
		[[nodiscard]] VkSampler GetDefaultSampler() const;

		void SetLastKnownLayout(VkImageLayout layout);
		void SetQueueFamilyOwner(std::uint32_t queueFamilyIndex);
		void SetVirtualResourceId(std::uint64_t virtualResourceId);

		explicit operator bool() const;

	private:
		static constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;

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
		BindlessManager* m_bindlessManager = nullptr;
		VkDevice m_bindlessDevice = VK_NULL_HANDLE;
		VkImageView m_defaultView = VK_NULL_HANDLE;
		VkSampler m_defaultSampler = VK_NULL_HANDLE;
		std::uint32_t m_bindlessSlot = kInvalidBindlessSlot;
	};
}
