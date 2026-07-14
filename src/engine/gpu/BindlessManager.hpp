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
		};

		BindlessManager() = default;
		~BindlessManager();

		BindlessManager(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");
		BindlessManager& operator=(const BindlessManager&) = AE_DELETE_MSG("BindlessManager manages GPU descriptor resources - use reference");

		Expected<void> Initialize(const VulkanContext& context, const Config& config);
		void Shutdown();

		void SetMemoryTracker(class GpuMemoryTracker* tracker);

		// recipe (not the VkImageView handle) - required by the extension.
		[[nodiscard]] Expected<void> WriteSampledImage(std::uint32_t slot, const void* viewCreateInfo, gpu::ImageLayout layout);

		void WriteLinearSampler();

		[[nodiscard]] gpu::DeviceAddress GetResourceHeapAddress() const;
		[[nodiscard]] gpu::DeviceAddress GetSamplerHeapAddress() const;
		[[nodiscard]] gpu::DeviceSize GetResourceHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetSamplerHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetImageDescriptorSize() const;

		[[nodiscard]] const void* GetDescriptorHeapMappings() const;

		void CmdBindHeaps(gpu::CommandList& cmd) const;

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

		mutable std::mutex m_mutex;
		gpu::Device m_device = nullptr;
		void* m_vmaAllocator = nullptr;

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

		std::uint32_t m_capacity = 0;
		std::uint32_t m_deferredFreeFrames = 3;
		std::uint64_t m_currentFrame = 0;
		std::vector<std::uint32_t> m_freeSlots;
		std::vector<bool> m_slotAllocated;
		std::vector<PendingSlotFree> m_pendingSlotFrees;
	};
} // namespace aether
