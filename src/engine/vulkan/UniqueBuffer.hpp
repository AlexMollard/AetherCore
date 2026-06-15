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
		// Engine-side overload: opaque gpu::Device / gpu::Allocator / gpu::BufferUsage / gpu::DeviceSize.
		// Optional gpu::MappedMemoryUsage controls VMA host-access flags
		// (sequential write + mapped by default).
		//
		// TODO(audit/P2b): these engine-side overloads are a bridge that keeps
		// the UniqueBuffer RAII path alive for the engine. The P2b Phase B
		// migration in docs/plans/resource-registry-consolidation.md replaces
		// UniqueBuffer members with gpu::BufferHandle + gpu::ResourceRegistry
		// factories (CreateMappedBuffer / ResolveMappedBuffer). New engine
		// code should prefer the registry directly.
		static Expected<UniqueBuffer> CreateMapped(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, const char* debugName = nullptr);
		static Expected<UniqueBuffer> CreateMapped(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, gpu::MappedMemoryUsage memoryUsage, const char* debugName = nullptr);

		// Vulkan-internal overload: raw VkDevice / VmaAllocator / VkBufferUsageFlags / VkDeviceSize.
		// Used by RenderGraphStorage and other backend code.
		static Expected<UniqueBuffer> CreateMapped(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName = nullptr);

		// Convenience factory: device-local, GPU-optimal. Caller must upload via staging.
		// Engine-side overload: opaque gpu::Device / gpu::Allocator / gpu::BufferUsage / gpu::DeviceSize.
		static Expected<UniqueBuffer> CreateDeviceLocal(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, const char* debugName = nullptr);

		// Vulkan-internal overload: raw VkDevice / VmaAllocator / VkBufferUsageFlags / VkDeviceSize.
		static Expected<UniqueBuffer> CreateDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName = nullptr);

		// Create a buffer aliased to an existing VmaAllocation at a given memory offset.
		// The buffer is bound via vkBindBufferMemory2. The caller owns the VmaAllocation lifetime.
		// Vulkan-internal only.
		static Expected<UniqueBuffer> CreateAliased(VkDevice device, VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset);

		void Reset();

		// Flushes host-visible memory to the device.
		// Returns Expected<void> - errors if the flush fails.
		// Note: gpu::DeviceSize == VkDeviceSize == uint64_t, so this single
		// overload accepts both. Engine callers pass gpu::DeviceSize literals.
		Expected<void> FlushMapped(gpu::DeviceSize offset = 0, gpu::DeviceSize size = VK_WHOLE_SIZE) const;

		[[nodiscard]] VkBuffer Get() const;
		// Engine-side accessor: returns the buffer handle as an opaque
		// gpu::Buffer so engine callers can pass it to engine-side
		// factories without mentioning Vk*.
		[[nodiscard]] gpu::Buffer GetBuffer() const noexcept
		{
			return static_cast<gpu::Buffer>(m_buffer);
		}
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
