#pragma once

#include <cstdint>
#include <mutex>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "GpuMaterial.hpp"

namespace aether
{
	class VulkanContext;

	// Manages a persistently-mapped GPU buffer that stores an array of GpuMaterial
	// records accessible via Buffer Device Address (BDA).
	//
	// Usage:
	//   uint32_t slot = buffer.AllocateSlot();
	//   buffer.Write(slot, gpuMaterial);
	//   // per-frame: pass buffer.GetDeviceAddress() in FrameConstants
	//   // per-draw:  pass slot as the materialIndex push constant
	//   buffer.FreeSlot(slot);   // when the material is no longer needed
	class MaterialBuffer
	{
	public:
		static constexpr std::uint32_t kMaxMaterials = 4096;
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		MaterialBuffer() = default;
		~MaterialBuffer();

		MaterialBuffer(const MaterialBuffer&) = delete;
		MaterialBuffer& operator=(const MaterialBuffer&) = delete;

		void Initialize(const VulkanContext& ctx);
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_device != VK_NULL_HANDLE;
		}

		// Allocate a free slot.  Returns kInvalidSlot when the buffer is full.
		[[nodiscard]] std::uint32_t AllocateSlot();

		// Release a slot so it can be reused.
		void FreeSlot(std::uint32_t slot);

		// Overwrite one material record in the persistent GPU mapping.
		// The write is immediately visible to all subsequent draw calls.
		void Write(std::uint32_t slot, const GpuMaterial& material);

		// Buffer Device Address; store this in FrameConstants every frame.
		[[nodiscard]] VkDeviceAddress GetDeviceAddress() const
		{
			return m_address;
		}

	private:
		mutable std::mutex m_mutex;
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation{};
		GpuMaterial* m_mapped = nullptr;
		VkDeviceAddress m_address = 0;
		std::vector<uint32_t> m_freeSlots;
	};
} // namespace aether
