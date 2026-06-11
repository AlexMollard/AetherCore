#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "gpu/GpuHandles.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	// ─────────────────────────────────────────────────────────────────────────
	// ResourceRegistry - Phase 2 of the GPU refactor
	// ─────────────────────────────────────────────────────────────────────────
	// Owns opaque handles for engine-facing GPU resources and centralizes
	// deferred destruction (3-frame WaitIdle-aware ring).
	//
	// Slot tables: TextureSlot, BufferSlot, PipelineSlot. Each slot is
	// {generation, std::optional<Entry>}. Generation increments on slot reuse
	// so stale handles fail validation in Resolve*().
	//
	// Deferred destruction: callers schedule destruction with Destroy(handle);
	// the entry is queued in the *current* frame's ring slot. AdvanceFrame()
	// promotes the ring by one slot and runs the destroyers that have been
	// in flight for at least kMaxFramesInFlight frames (i.e. the GPU is
	// guaranteed to be done with them because GpuDevice::AdvanceBindlessFrame
	// and GpuDevice::WaitIdle both block on the same fence).
	//
	// This is the foundation Phase 3+ code uses to expose handle-based GPU
	// resources at the engine boundary. Existing UniqueBuffer/UniqueImage
	// wrappers continue to do immediate teardown (their consumers don't hold
	// buffers across frames); the registry's ring is reserved for new code
	// paths that need the centralized lifetime guarantee.
	class ResourceRegistry
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

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
		};

		ResourceRegistry() = default;
		~ResourceRegistry();

		ResourceRegistry(const ResourceRegistry&) = delete;
		ResourceRegistry& operator=(const ResourceRegistry&) = delete;

		void Shutdown();

		// Texture registration. The registry does NOT take ownership of the
		// VkImage/VkImageView by default (ownsAllocation=false); pass
		// ownsAllocation=true when the caller wants the registry to destroy
		// them via Destroy(handle) -> WaitIdle-aware deferred path.
		gpu::TextureHandle RegisterTexture(const TextureEntry& entry);

		// Buffer registration. Same ownership semantics as TextureEntry.
		gpu::BufferHandle RegisterBuffer(const BufferEntry& entry);

		// Pipeline registration. Layouts may be shared; the registry destroys
		// them only when ownsLayout is true.
		gpu::PipelineHandle RegisterPipeline(const PipelineEntry& entry);

		// Schedule destruction of the resource backing the handle. The actual
		// destruction runs kMaxFramesInFlight frames later (in AdvanceFrame).
		// Stale handles (wrong generation) are silently ignored - the
		// generation check that would have caught the use is in IsValid().
		void Destroy(gpu::TextureHandle handle);
		void Destroy(gpu::BufferHandle handle);
		void Destroy(gpu::PipelineHandle handle);

		// Resolve handle -> entry. Returns nullptr if the handle is stale or
		// the slot is empty. Never null on a freshly-registered, valid handle.
		[[nodiscard]] const TextureEntry* Resolve(gpu::TextureHandle handle) const;
		[[nodiscard]] const BufferEntry* Resolve(gpu::BufferHandle handle) const;
		[[nodiscard]] const PipelineEntry* Resolve(gpu::PipelineHandle handle) const;

		// Direct mutating access for the backend to update bindless slots or
		// storage views after registration (e.g. when a transient image gets
		// promoted to bindless-sampled).
		[[nodiscard]] TextureEntry* ResolveMutable(gpu::TextureHandle handle);

		// Tick the deferred-destruction ring forward by one frame. Must be
		// called once per frame by GpuDevice::BeginSwapchainFrame. After this
		// call, the ring slot for the frame the GPU just finished holds
		// resources that are safe to destroy (GpuDevice::WaitIdle at the
		// corresponding point guarantees the GPU has retired them).
		void AdvanceFrame();

		// Called by GpuDevice::Shutdown to ensure all queued destroyers run
		// while the device is still valid. After this, all slot tables are
		// empty and the ring has been drained.
		void DrainAll();

		// Phase-A consolidation: device/allocator back-references so the
		// registry can allocate and map buffers directly instead of relying
		// on a parallel side-channel in the gpu/ bridge.
		void Init(VkDevice device, VmaAllocator allocator) noexcept;

		[[nodiscard]] gpu::BufferHandle CreateBuffer(const gpu::BufferDesc& desc) noexcept;
		[[nodiscard]] gpu::BufferHandle CreateMappedBuffer(const gpu::MappedBufferDesc& desc) noexcept;
		[[nodiscard]] gpu::TextureHandle CreateTexture(const gpu::TextureDesc& desc) noexcept;

		[[nodiscard]] gpu::MappedBufferView ResolveMappedBuffer(gpu::BufferHandle handle) const noexcept;
		void FlushMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept;

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
			std::uint32_t slotIndex = 0;
			std::uint32_t generation = 0;
			DestructionFn fn;
		};

		// Per-slot storage. std::optional so an empty slot costs only the
		// size of the bool + alignment padding. Generation lives next to the
		// payload so any reuse bumps the generation atomically.
		struct TextureSlot
		{
			std::uint32_t generation = 1; // start at 1 so 0 is "never used"
			std::optional<TextureEntry> entry;
		};

		struct BufferSlot
		{
			std::uint32_t generation = 1;
			std::optional<BufferEntry> entry;
		};

		struct PipelineSlot
		{
			std::uint32_t generation = 1;
			std::optional<PipelineEntry> entry;
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

		std::vector<PendingDestruction> m_pendingDestructions[kMaxFramesInFlight];
		std::uint32_t m_currentFrame = 0;

		bool m_shutdown = false;

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
	};
} // namespace aether
