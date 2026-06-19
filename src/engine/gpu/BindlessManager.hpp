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

	// Bindless descriptor heap manager.
	//
	// Manages two descriptor heaps (VK_EXT_descriptor_heap):
	//   - Resource heap: SAMPLED_IMAGE descriptors for bindless textures.
	//   - Sampler heap:  one immutable linear SAMPLER shared by all draws.
	//
	// Descriptors live at byte offsets inside the heap buffers. At draw time,
	// the heaps are bound via vkCmdBindResourceHeapEXT / vkCmdBindSamplerHeapEXT
	// and shaders index into them by offset (either constant or push-data-driven).
	//
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

		// Diagnostic address tracking. Registers both descriptor heap
		// buffers with the GpuMemoryTracker so fault addresses inside the
		// bindless heaps resolve to "bindless_resource_heap" /
		// "bindless_sampler_heap" in the diagnostic dashboard.
		void SetMemoryTracker(class GpuMemoryTracker* tracker);

		// Write a SAMPLED_IMAGE descriptor into the resource heap at the given slot.
		// The descriptor data is written directly into the mapped heap via
		// vkWriteResourceDescriptorsEXT. viewCreateInfo is the VkImageViewCreateInfo
		// recipe (not the VkImageView handle) - required by the extension.
		[[nodiscard]] Expected<void> WriteSampledImage(std::uint32_t slot, const void* viewCreateInfo, gpu::ImageLayout layout);

		// Write the linear sampler into the sampler heap (one-time init / on-demand).
		void WriteLinearSampler();

		// Heap properties (for pipeline mapping and per-frame bind).
		[[nodiscard]] gpu::DeviceAddress GetResourceHeapAddress() const;
		[[nodiscard]] gpu::DeviceAddress GetSamplerHeapAddress() const;
		[[nodiscard]] gpu::DeviceSize GetResourceHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetSamplerHeapSize() const;
		[[nodiscard]] gpu::DeviceSize GetImageDescriptorSize() const;

		// Pipeline mapping info for bindless textures/samplers.
		// Returns a const void* to a VkShaderDescriptorSetAndBindingMappingInfoEXT
		// that chains into VkPipelineShaderStageCreateInfo::pNext for every shader
		// stage that accesses bindless resource declarations (g_textures[], g_linearSampler).
		// Cast to const VkShaderDescriptorSetAndBindingMappingInfoEXT* in the vulkan layer.
		[[nodiscard]] const void* GetDescriptorHeapMappings() const;

		// Bind both resource and sampler heaps on the command buffer.
		// Called once per-pass/per-frame BEFORE any draw or dispatch that
		// accesses bindless textures/samplers.
		void CmdBindHeaps(gpu::CommandList& cmd) const;

		// No-cache one-shot VkSampler creation. Returns a gpu::Sampler that
		// must be destroyed by the caller via vkDestroySampler. Used by
		// subsystems that need per-pipeline samplers outside the descriptor heap
		// (e.g. LocalShadowService blur sampler).
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
		void WriteLinearSamplerUnlocked();

		mutable std::mutex m_mutex;
		gpu::Device m_device = nullptr;
		void* m_vmaAllocator = nullptr;

		// ── Descriptor heaps (VK_EXT_descriptor_heap) ──
		void* m_resourceHeapBuffer = nullptr;
		void* m_resourceHeapAlloc = nullptr;
		void* m_resourceHeapMapped = nullptr;
		gpu::DeviceAddress m_resourceHeapAddr = 0;
		gpu::DeviceSize m_resourceHeapSize = 0;
		gpu::DeviceSize m_imageDescriptorSize = 0;
		gpu::DeviceSize m_imageDescriptorAlignment = 0;

		void* m_samplerHeapBuffer = nullptr;
		void* m_samplerHeapAlloc = nullptr;
		void* m_samplerHeapMapped = nullptr;
		gpu::DeviceAddress m_samplerHeapAddr = 0;
		gpu::DeviceSize m_samplerHeapSize = 0;
		gpu::DeviceSize m_samplerDescriptorSize = 0;
		gpu::DeviceSize m_samplerDescriptorAlignment = 0;

		GpuMemoryTracker* m_memoryTracker = nullptr;

		// Pipeline mapping storage (opaque VkDescriptorSetAndBindingMappingEXT arrays).
		// Allocated in Initialize(), freed in Shutdown().
		void* m_shaderMappingInfo = nullptr;

		// ── Slot management ──
		std::uint32_t m_capacity = 0;
		std::uint32_t m_deferredFreeFrames = 3;
		std::uint64_t m_currentFrame = 0;
		std::vector<std::uint32_t> m_freeSlots;
		std::vector<bool> m_slotAllocated;
		std::vector<PendingSlotFree> m_pendingSlotFrees;
	};
} // namespace aether
