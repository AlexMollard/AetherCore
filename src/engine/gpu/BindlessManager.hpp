#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Assert.hpp"

namespace aether
{
	class VulkanContext;

	// Thread-safe: all public methods lock a mutex.
	class BindlessManager
	{
	public:
		struct Config
		{
			std::uint32_t maxSampledImages = 4096;
			std::uint32_t deferredFreeFrames = 3;
			// Texture samples along the footprint when a surface is seen edge-on. Clamped
			// to the device ceiling; 1 disables anisotropic filtering.
			std::uint32_t maxAnisotropy = 16;
		};

		BindlessManager() = default;
		~BindlessManager();

		BindlessManager(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");
		BindlessManager& operator=(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");

		Expected<void> Initialize(const VulkanContext& context, const Config& config);
		void Shutdown();

		void SetMemoryTracker(class GpuMemoryTracker* tracker);

		// The descriptor-heap path takes the VkImageViewCreateInfo recipe (not the handle) -
		// required by the extension. The descriptor-set fallback needs the real VkImageView
		// instead, so callers pass both; each backend uses the one it can.
		[[nodiscard]] Expected<void> WriteSampledImage(std::uint32_t slot, const void* viewCreateInfo, const void* imageView, gpu::ImageLayout layout);

		void WriteLinearSampler();

		// Change the anisotropy every sampler is created with, and rewrite the one
		// shader-visible sampler so the change takes effect without recreating the device.
		// Returns false when the value was already in force (or was clamped to it).
		//
		// The CALLER must have waited for the device to be idle: this overwrites a descriptor
		// the GPU reads and may destroy a sampler object it is still using.
		bool SetMaxAnisotropy(std::uint32_t requested);

		[[nodiscard]] gpu::DeviceAddress GetResourceHeapAddress() const;
		[[nodiscard]] gpu::DeviceAddress GetSamplerHeapAddress() const;
		[[nodiscard]] gpu::DeviceSize GetResourceHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetSamplerHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetImageDescriptorSize() const;

		[[nodiscard]] const void* GetDescriptorHeapMappings() const;

		// Binds whatever the active backend needs before draws: the two heaps, or the one
		// global descriptor set.
		void CmdBindGlobalResources(gpu::CommandList& cmd) const;

		// True when the device had VK_EXT_descriptor_heap. False means the update-after-bind
		// descriptor-set fallback is live - same slots, same shaders, different plumbing.
		[[nodiscard]] bool UsesDescriptorHeap() const;

		// must be destroyed by the caller via vkDestroySampler. Used by
		[[nodiscard]] Expected<gpu::Sampler> CreateSampler(gpu::Filter filter, gpu::SamplerMipmapMode mipmap, gpu::SamplerAddressMode address) const;

		[[nodiscard]] std::uint32_t GetCapacity() const;
		[[nodiscard]] Expected<std::uint32_t> AllocateSampledImageSlot();
		void FreeSampledImageSlot(std::uint32_t slot);
		void FreeSampledImageSlotDeferred(std::uint32_t slot);
		void AdvanceFrame(std::uint64_t frameIndex);

	private:
		struct PendingSlotFree
		{
			std::uint32_t slot = 0;
			std::uint64_t releaseFrame = 0;
		};

		void FreeSlotImmediateUnlocked(std::uint32_t slot);
		void ShutdownUnlocked();
		void WriteLinearSamplerUnlocked();
		[[nodiscard]] Expected<void> InitializeDescriptorBufferBackendUnlocked(const VulkanContext& context);
		void ShutdownDescriptorBufferBackendUnlocked();

		mutable std::mutex m_mutex;
		gpu::Device m_device = nullptr;
		void* m_vmaAllocator = nullptr;
		// What samplers are actually created with: the requested setting clamped to the
		// hardware ceiling below. 1.0 means no anisotropic filtering.
		float m_maxAnisotropy = 1.0f;
		// The hardware ceiling itself, kept so a later request can be re-clamped.
		float m_deviceMaxAnisotropy = 1.0f;

		void* m_resourceHeapBuffer = nullptr;
		void* m_resourceHeapAlloc = nullptr;
		void* m_resourceHeapMapped = nullptr;
		gpu::DeviceAddress m_resourceHeapAddr = 0;
		gpu::DeviceSize m_resourceHeapSize = 0;
		gpu::DeviceSize m_resourceHeapReservedRangeOffset = 0;
		gpu::DeviceSize m_resourceHeapReservedRangeSize = 0;
		gpu::DeviceSize m_imageDescriptorSize = 0;
		gpu::DeviceSize m_imageDescriptorAlignment = 0;
		gpu::DeviceSize m_imageDescriptorStride = 0;

		void* m_samplerHeapBuffer = nullptr;
		void* m_samplerHeapAlloc = nullptr;
		void* m_samplerHeapMapped = nullptr;
		gpu::DeviceAddress m_samplerHeapAddr = 0;
		gpu::DeviceSize m_samplerHeapSize = 0;
		gpu::DeviceSize m_samplerHeapReservedRangeOffset = 0;
		gpu::DeviceSize m_samplerHeapReservedRangeSize = 0;
		gpu::DeviceSize m_samplerDescriptorSize = 0;
		gpu::DeviceSize m_samplerDescriptorAlignment = 0;

		GpuMemoryTracker* m_memoryTracker = nullptr;

		void* m_shaderMappingInfo = nullptr;

		// Descriptor-buffer fallback (all null/zero while the heap backend is active). The
		// descriptors themselves reuse m_resourceHeapBuffer/Mapped/Addr - both backends store
		// descriptors in one mapped buffer, only the API that writes and binds them differs.
		// Handles are void* to keep this header free of the Vulkan headers, as elsewhere here.
		bool m_useDescriptorHeap = true;
		void* m_setLayout = nullptr;
		void* m_pipelineLayout = nullptr;
		void* m_fallbackSampler = nullptr;
		gpu::DeviceSize m_imageBindingOffset = 0;
		gpu::DeviceSize m_samplerBindingOffset = 0;

		std::uint32_t m_capacity = 0;
		std::uint32_t m_deferredFreeFrames = 3;
		std::uint64_t m_currentFrame = 0;
		std::vector<std::uint32_t> m_freeSlots;
		std::vector<bool> m_slotAllocated;
		std::vector<PendingSlotFree> m_pendingSlotFrees;
	};
} // namespace aether
