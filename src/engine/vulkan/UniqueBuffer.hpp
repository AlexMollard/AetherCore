#pragma once

#include <cstdint>
#include <vk_mem_alloc.h>

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
		static Expected<UniqueBuffer> CreateMapped(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage);

		// Convenience factory: device-local, GPU-optimal. Caller must upload via staging.
		static Expected<UniqueBuffer> CreateDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage);

		void Reset();

		// Flushes host-visible memory to the device.
		// Returns Expected<void> - errors if the flush fails.
		Expected<void> FlushMapped(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

		[[nodiscard]] VkBuffer Get() const;
		[[nodiscard]] VmaAllocation GetAllocation() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] const VmaAllocationInfo& GetAllocationInfo() const;
		[[nodiscard]] VkDeviceAddress GetDeviceAddress() const;
		[[nodiscard]] VkBufferUsageFlags GetUsage() const;
		[[nodiscard]] VkDeviceSize GetSize() const;
		[[nodiscard]] bool HasDeviceAddress() const;
		[[nodiscard]] std::uint64_t GetVirtualResourceId() const;

		void SetVirtualResourceId(std::uint64_t virtualResourceId);

		explicit operator bool() const;

	private:
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = VK_NULL_HANDLE;
		VmaAllocationInfo m_allocationInfo{};
		VkBufferUsageFlags m_usage = 0;
		VkDeviceSize m_size = 0;
		VkDeviceAddress m_deviceAddress = 0;
		std::uint64_t m_virtualResourceId = 0;
	};
} // namespace aether
