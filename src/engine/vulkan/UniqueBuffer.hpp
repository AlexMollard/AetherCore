#pragma once

#include <cstdint>
#include <vk_mem_alloc.h>

#include "gpu/GpuTypes.hpp"
#include "utils/Assert.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	class UniqueBuffer
	{
	public:
		UniqueBuffer() = default;
		~UniqueBuffer();

		UniqueBuffer(const UniqueBuffer&) = AE_DELETE_MSG("UniqueBuffer owns a VkBuffer - use std::move");
		UniqueBuffer& operator=(const UniqueBuffer&) = AE_DELETE_MSG("UniqueBuffer owns a VkBuffer - use std::move");

		UniqueBuffer(UniqueBuffer&& other) noexcept;
		UniqueBuffer& operator=(UniqueBuffer&& other) noexcept;

		static Expected<UniqueBuffer> Create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& bufferCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo);

		// Convenience factory: persistently-mapped, host-sequential-write, coherency-flushed.
		// Use for per-frame CPU-written buffers (uniform/storage).
		static Expected<UniqueBuffer> CreateMapped(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName = nullptr);

		// Convenience factory: device-local, GPU-optimal. Caller must upload via staging.
		static Expected<UniqueBuffer> CreateDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName = nullptr);

		// Create a buffer aliased to an existing VmaAllocation at a given memory offset.
		// The buffer is bound via vkBindBufferMemory2. The caller owns the VmaAllocation lifetime.
		static Expected<UniqueBuffer> CreateAliased(VkDevice device, VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset);

		void Reset();

		// Flushes host-visible memory to the device.
		// Returns Expected<void> - errors if the flush fails.
		Expected<void> FlushMapped(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

		[[nodiscard]] VkBuffer Get() const;
		[[nodiscard]] VmaAllocation GetAllocation() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] const VmaAllocationInfo& GetAllocationInfo() const;
		[[nodiscard]] gpu::DeviceAddress GetDeviceAddress() const;
		[[nodiscard]] VkBufferUsageFlags GetUsage() const;
		[[nodiscard]] VkDeviceSize GetSize() const;
		[[nodiscard]] bool HasDeviceAddress() const;
		[[nodiscard]] std::uint64_t GetVirtualResourceId() const;

		void SetVirtualResourceId(std::uint64_t virtualResourceId);

		// Name this buffer for RenderDoc / NSight / validation layers.
		// No-op if the debug utils extension was not loaded.
		void SetName(const char* name) const;

		// Called once by VulkanContext after device creation.
		static void SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn);

		explicit operator bool() const;

	private:
		static inline PFN_vkSetDebugUtilsObjectNameEXT s_setObjectNameFn = nullptr;

		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = VK_NULL_HANDLE;
		VmaAllocationInfo m_allocationInfo{};
		bool m_ownsAllocation = true;
		VkBufferUsageFlags m_usage = 0;
		VkDeviceSize m_size = 0;
		gpu::DeviceAddress m_deviceAddress = 0;
		std::uint64_t m_virtualResourceId = 0;
	};
} // namespace aether
