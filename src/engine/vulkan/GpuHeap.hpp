#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include "gpu/GpuTypes.hpp"
#include "vulkan/GpuSpan.hpp"
#include "vulkan/volk.hpp"

namespace aether::vulkan
{
	class TransferManager;
}

namespace aether
{
	class VulkanContext;
	class GpuMemoryTracker;

	// allocator: best-fit, coalescing, alignment - same pattern as
	class GpuHeap
	{
	public:
		struct Desc
		{
			gpu::DeviceSize capacityBytes = 256ull * 1024 * 1024;
			gpu::BufferUsage additionalUsage = gpu::BufferUsage::None;
			const char* debugName = nullptr;
		};

		GpuHeap() = default;

		~GpuHeap()
		{
			Shutdown();
		}

		GpuHeap(const GpuHeap&) = delete;
		GpuHeap& operator=(const GpuHeap&) = delete;
		GpuHeap(GpuHeap&&) = delete;
		GpuHeap& operator=(GpuHeap&&) = delete;

		void Initialize(const VulkanContext& ctx, Desc desc);
		void Shutdown();

		void SetMemoryTracker(GpuMemoryTracker* tracker);

		template<typename T>
		[[nodiscard]] GpuSpan<T> Alloc(std::uint32_t count)
		{
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(T);
			const VkDeviceSize offset = AllocBytes(bytes);
			if (offset == kInvalidOffset)
			{
				return {};
			}
			GpuSpan<T> span;
			span.data = nullptr;
			span.address = m_baseAddress + offset;
			span.count = count;
			return span;
		}

		template<typename T>
		void Free(GpuSpan<T>& span)
		{
			if (!span.IsValid())
			{
				return;
			}
			FreeBytes(span.address);
			span = {};
		}

		// Non-blocking upload via the transfer queue: returns the transfer ticket the
		// copies signal (the frame submission waits on it; the staging buffer is
		// reclaimed once it completes).
		template<typename T>
		std::uint64_t Upload(GpuSpan<T> dst, std::span<const T> src, vulkan::TransferManager& transfer)
		{
			return UploadBytes(dst.address, src.data(), static_cast<VkDeviceSize>(src.size()) * sizeof(T), transfer);
		}

		[[nodiscard]] VkBuffer GetBuffer() const
		{
			return m_buffer;
		}

		[[nodiscard]] gpu::DeviceAddress GetBaseAddress() const
		{
			return m_baseAddress;
		}

		template<typename T>
		[[nodiscard]] VkDeviceSize GetOffset(GpuSpan<T> span) const
		{
			return static_cast<VkDeviceSize>(span.address - m_baseAddress);
		}

	private:
		static constexpr VkDeviceSize kInvalidOffset = ~0ull;

		VkBuffer m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_bufferAllocation = VK_NULL_HANDLE;
		VmaVirtualBlock m_virtualBlock = VK_NULL_HANDLE;
		gpu::DeviceAddress m_baseAddress = 0;
		VkDeviceSize m_capacityBytes = 0;
		VmaAllocator m_allocatorRef = nullptr;
		VkDevice m_deviceRef = VK_NULL_HANDLE;
		GpuMemoryTracker* m_memoryTracker = nullptr;
		std::string m_debugName;
		std::unordered_map<gpu::DeviceAddress, VmaVirtualAllocation> m_allocations;

		VkDeviceSize AllocBytes(VkDeviceSize bytes);
		void FreeBytes(gpu::DeviceAddress addr);
		std::uint64_t UploadBytes(gpu::DeviceAddress dstAddr, const void* src, VkDeviceSize bytes, vulkan::TransferManager& transfer);
	};
} // namespace aether
