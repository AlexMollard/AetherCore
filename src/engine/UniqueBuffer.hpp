#pragma once

#include <cstdint>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace aether
{
	class UniqueBuffer
	{
	public:
		UniqueBuffer() = default;
		~UniqueBuffer();

		UniqueBuffer(const UniqueBuffer&) = delete;
		UniqueBuffer& operator=(const UniqueBuffer&) = delete;

		UniqueBuffer(UniqueBuffer&& other) noexcept;
		UniqueBuffer& operator=(UniqueBuffer&& other) noexcept;

		static UniqueBuffer Create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& bufferCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo);

		// Convenience factory: persistently-mapped, host-sequential-write, coherency-flushed.
		// Use for per-frame CPU-written buffers (uniform/storage).
		static UniqueBuffer CreateMapped(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage);

		// Convenience factory: device-local, GPU-optimal. Caller must upload via staging.
		static UniqueBuffer CreateDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage);

		void Reset();

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
