#pragma once

#include <cstdint>
#include <vk_mem_alloc.h>

#include "utils/Assert.hpp"
#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class BindlessManager;

	class UniqueImage
	{
	public:
		// Concise description for the common-case Create overload.
		// Aspect is deduced from the format; sampler is not created automatically.
		// Engine-side: usage is gpu::ImageUsage (bit-OR of flags). Translates to
		// VkImageUsageFlags at the seam. samples / memoryUsage are vulkan-internal
		// overrides used by RenderGraphStorage's transient heap allocator only;
		// engine callers can leave them at their defaults.
		struct Desc
		{
			gpu::Extent2D extent = {};
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			std::uint32_t mipLevels = 1;
			std::uint32_t arrayLayers = 1;
			// Vulkan-internal fields - used by RenderGraphStorage when allocating
			// from the transient heap. Engine callers should leave at defaults.
			VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
			VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			const char* debugName = nullptr;
		};
		// TextureFilter moved to gpu/GpuEnums.hpp so engine code can use it
		// without including the vulkan backend header.

		UniqueImage() = default;
		~UniqueImage();

		UniqueImage(const UniqueImage&) = AE_DELETE_MSG("UniqueImage owns a VkImage - use std::move");
		UniqueImage& operator=(const UniqueImage&) = AE_DELETE_MSG("UniqueImage owns a VkImage - use std::move");

		UniqueImage(UniqueImage&& other) noexcept;
		UniqueImage& operator=(UniqueImage&& other) noexcept;

		// High-level overload: builds VkImageCreateInfo from Desc, allocates via VMA,
		// and creates the default VkImageView automatically.  Aspect is deduced from
		// the format; device is stored so Reset() can destroy the view.
		// Engine-side opaque handles: gpu::Device / gpu::Allocator.
		//
		// TODO(audit/P2b): this engine-side overload is a bridge that keeps
		// the UniqueImage RAII path alive for the engine. The P2b Phase B
		// migration in docs/plans/resource-registry-consolidation.md replaces
		// UniqueImage members with gpu::TextureHandle + gpu::ResourceRegistry
		// factories (CreateTexture). New engine code should prefer the registry
		// directly.
		static Expected<UniqueImage> Create(gpu::Device device, gpu::Allocator allocator, const Desc& desc);

		// Vulkan-internal overload: raw VkDevice / VmaAllocator for callers that
		// already have them (RenderGraphStorage, Swapchain, etc.). Prefer the
		// engine-side overload above for any engine code.
		static Expected<UniqueImage> Create(VkDevice device, VmaAllocator allocator, const Desc& desc);

		// Low-level overload: caller supplies the full Vulkan structs directly.
		// No default view is created; callers manage their own VkImageViews.
		// Vulkan-internal only.
		static Expected<UniqueImage> Create(VmaAllocator allocator, const VkImageCreateInfo& imageCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo);

		// Create an image aliased to an existing VmaAllocation at a given memory offset.
		// The image is created with VK_IMAGE_CREATE_ALIAS_BIT and bound via
		// vkBindImageMemory2. The caller owns the VmaAllocation lifetime.
		// Vulkan-internal only.
		static Expected<UniqueImage> CreateAliased(VkDevice device, VmaAllocator allocator, const Desc& desc, VmaAllocation existingAllocation, VkDeviceSize memoryOffset);

		void Reset();

		// Engine-side: register the image as a bindless sampled image using opaque
		// gpu:: handles. Translates to VkDevice + VkImageAspectFlags + VkImageLayout
		// at the seam. Engine callers use this.
		Expected<void> EnsureBindlessSampled(
		        BindlessManager& bindlessManager, gpu::Device device, gpu::ImageAspect aspectMask = gpu::ImageAspect::Color, gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly, TextureFilter filter = TextureFilter::Linear);

		// Vulkan-internal overload: raw VkDevice / VkImageAspectFlags / VkImageLayout.
		// Used by RenderGraphStorage and other backend code. Prefer the engine-side
		// overload above for any engine code.
		Expected<void> EnsureBindlessSampled(
		        BindlessManager& bindlessManager, VkDevice device, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, VkImageLayout descriptorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, TextureFilter filter = TextureFilter::Linear);

		void ReleaseBindlessSampled(bool deferSlotFree = true);

		[[nodiscard]] VkImage Get() const;
		// Engine-side accessor: returns the image handle as an opaque
		// gpu::ImageView so engine callers can pass it to engine-side
		// factories without mentioning Vk*.
		[[nodiscard]] gpu::ImageView GetImageView() const noexcept
		{
			return static_cast<gpu::ImageView>(m_image);
		}
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

		// Name this image (and its default view, if present) for RenderDoc / validation layers.
		// No-op when the debug-utils extension was not loaded.
		void SetName(VkDevice device, const char* name) const;

		explicit operator bool() const;

	private:
		static constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;

		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkImage m_image = VK_NULL_HANDLE;
		VmaAllocation m_allocation = VK_NULL_HANDLE;
		VmaAllocationInfo m_allocationInfo{};
		bool m_ownsAllocation = true;
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
} // namespace aether
