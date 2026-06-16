#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "gpu/GpuHandles.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class BindlessManager;
}

namespace aether
{
	// Owns opaque handles for engine-facing GPU resources and centralizes
	// deferred destruction (kMaxFramesInFlight ring, WaitIdle-aware).
	//
	// Slot tables: TextureSlot, BufferSlot, PipelineSlot. Each slot is
	// {generation, std::optional<Entry>}. Generation increments on slot reuse
	// so stale handles fail validation in Resolve*().
	//
	// Deferred destruction: callers schedule destruction with Destroy(handle);
	// the entry is queued in the *current* frame's ring slot. AdvanceFrame()
	// promotes the ring by one slot and runs destroyers whose target frame has
	// retired (GpuDevice::WaitIdle at the matching point guarantees GPU done).
	class ResourceRegistry
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

		// Debug backtrace constants.
		static constexpr int kAllocFrames = 4;
		static constexpr int kBacktraceDepth = 9;

		struct TextureEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageView storageView = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkExtent2D extent{};
			VkImageUsageFlags usage = 0;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			VkDevice device = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			bool ownsAllocation = false;
			bool ownsView = true;
			bool ownsStorageView = false;
			std::uint32_t mipLevels = 1;
			std::uint32_t arrayLayers = 1;
			static constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
			std::uint32_t bindlessSampledSlot = kInvalidBindlessSlot;
			bool hasBindlessSampled = false;
		};

		struct BufferEntry
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			VkBufferUsageFlags usage = 0;
			VkDeviceSize size = 0;
			bool ownsAllocation = false;
			void* mappedPtr = nullptr;
			VkDeviceAddress deviceAddress = 0;
		};

		struct PipelineEntry
		{
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			bool ownsLayout = false;
			// Optional GPL libraries; VK_NULL_HANDLE for compute / non-GPL pipelines.
			VkPipeline vertInputLib = VK_NULL_HANDLE;
			VkPipeline preRasterLib = VK_NULL_HANDLE;
			VkPipeline fragShaderLib = VK_NULL_HANDLE;
			VkPipeline fragOutputLib = VK_NULL_HANDLE;
		};

		ResourceRegistry() = default;
		~ResourceRegistry();

		ResourceRegistry(const ResourceRegistry&) = delete;
		ResourceRegistry& operator=(const ResourceRegistry&) = delete;

		void Shutdown();

		// Texture registration. Pass ownsAllocation=true to have the registry destroy on Destroy().
		gpu::TextureHandle RegisterTexture(const TextureEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Buffer registration.
		gpu::BufferHandle RegisterBuffer(const BufferEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Pipeline registration.
		gpu::PipelineHandle RegisterPipeline(const PipelineEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Schedule destruction. Runs kMaxFramesInFlight frames later in AdvanceFrame.
		void Destroy(gpu::TextureHandle handle);
		void Destroy(gpu::BufferHandle handle);
		void Destroy(gpu::PipelineHandle handle);

		// Resolve handle -> entry, or nullptr if stale.
		[[nodiscard]] const TextureEntry* Resolve(gpu::TextureHandle handle) const;
		[[nodiscard]] const BufferEntry* Resolve(gpu::BufferHandle handle) const;
		[[nodiscard]] const PipelineEntry* Resolve(gpu::PipelineHandle handle) const;

		[[nodiscard]] TextureEntry* ResolveMutable(gpu::TextureHandle handle);
		[[nodiscard]] BufferEntry* ResolveMutable(gpu::BufferHandle handle);

		// Advance the deferred-destruction ring by one frame.
		void AdvanceFrame();

		// Run all queued destroyers while the device is still valid.
		void DrainAll();

		void Init(VkDevice device, VmaAllocator allocator) noexcept;

		[[nodiscard]] gpu::BufferHandle CreateBuffer(const gpu::BufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] gpu::BufferHandle CreateMappedBuffer(const gpu::MappedBufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] gpu::TextureHandle CreateTexture(const gpu::TextureDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		[[nodiscard]] gpu::MappedBufferView ResolveMappedBuffer(gpu::BufferHandle handle) const noexcept;
		void FlushMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept;

		void SetBufferName(gpu::BufferHandle handle, const char* name);
		void SetTextureName(gpu::TextureHandle handle, const char* name);

		[[nodiscard]] gpu::BufferHandle CreateAliasedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName = {});
		[[nodiscard]] gpu::TextureHandle CreateAliasedTexture(const gpu::TextureDesc& desc, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName = {});

		// Bindless registration. BindlessManager pointer is for deferred slot-free on Destroy().
		void SetBindlessManager(BindlessManager* mgr);
		Expected<void> EnsureBindlessSampled(gpu::TextureHandle handle,
		        BindlessManager& bindlessManager,
		        gpu::ImageAspect aspectMask = gpu::ImageAspect::Color,
		        gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly,
		        TextureFilter filter = TextureFilter::Linear,
		        gpu::SamplerAddressMode addressMode = gpu::SamplerAddressMode::Repeat);
		[[nodiscard]] bool HasBindlessSampled(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(gpu::TextureHandle handle) const;

		[[nodiscard]] gpu::Format GetTextureFormat(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::Extent2D GetTextureExtent(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureMipLevels(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureArrayLayers(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::ImageUsage GetTextureUsage(gpu::TextureHandle handle) const;

		[[nodiscard]] gpu::DeviceSize GetBufferSize(gpu::BufferHandle handle) const;
		[[nodiscard]] gpu::BufferUsage GetBufferUsage(gpu::BufferHandle handle) const;

		// Diagnostic counters (not performance-critical, kept simple).
		[[nodiscard]] std::uint32_t LiveTextureCount() const;
		[[nodiscard]] std::uint32_t LiveBufferCount() const;
		[[nodiscard]] std::uint32_t LivePipelineCount() const;

	private:
		// A queued destructor. Holds a typed-owning variant of the resource
		// payload (or a closure) so destruction is unambiguous regardless of
		// ownership flags. We use a std::function to keep the type simple
		// at the cost of a small heap allocation per Destroy() call; this is
		// cold path (shaders/transients), not the steady-state hot loop.
		using DestructionFn = std::function<void()>;

		struct PendingDestruction
		{
			DestructionFn fn;
		};

		// Per-slot storage. std::optional so an empty slot costs only the
		// size of the bool + alignment padding. Generation lives next to the
		// payload so any reuse bumps the generation atomically.
#ifndef NDEBUG
		struct AllocFrame
		{
			std::source_location site;
			std::array<void*, kBacktraceDepth> addresses{};
			int frameCount = 0;
		};
#endif

		struct TextureSlot
		{
			std::uint32_t generation = 1; // start at 1 so 0 is "never used"
			std::optional<TextureEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0; // total pushes (may exceed kAllocFrames)
#endif
		};

		struct BufferSlot
		{
			std::uint32_t generation = 1;
			std::optional<BufferEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0;
#endif
		};

		struct PipelineSlot
		{
			std::uint32_t generation = 1;
			std::optional<PipelineEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0;
#endif
		};

		// Find an empty slot, or pick a victim for reuse. Returns ~0u when
		// the table is full (24-bit address space exhausted).
		[[nodiscard]] std::uint32_t AcquireTextureSlot();
		[[nodiscard]] std::uint32_t AcquireBufferSlot();
		[[nodiscard]] std::uint32_t AcquirePipelineSlot();

		void RunDestroyersInRing(std::vector<PendingDestruction>& ring);
		void DestroyTextureEntryNow(const TextureEntry& entry);
		void DestroyBufferEntryNow(const BufferEntry& entry);
		void DestroyPipelineEntryNow(const PipelineEntry& entry);

		std::vector<TextureSlot> m_textures;
		std::vector<BufferSlot> m_buffers;
		std::vector<PipelineSlot> m_pipelines;

		std::vector<std::uint32_t> m_freeTextureSlots;
		std::vector<std::uint32_t> m_freeBufferSlots;
		std::vector<std::uint32_t> m_freePipelineSlots;

		std::vector<PendingDestruction> m_pendingDestructions[kMaxFramesInFlight];
		std::uint32_t m_currentFrame = 0;

		bool m_shutdown = false;

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		BindlessManager* m_bindlessManager = nullptr;

		std::uint32_t m_liveTextureCount = 0;
		std::uint32_t m_liveBufferCount = 0;
		std::uint32_t m_livePipelineCount = 0;
	};
} // namespace aether
