#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>

#include "gpu/GpuTypes.hpp"
#include "vulkan/GpuSpan.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	class VulkanContext;

	// A device-local GPU memory arena backed by a single large VkBuffer.
	// Suballocates typed regions via a sorted free-list with coalescing.
	//
	// Think of it as GPU malloc: Alloc<T> is new[], Free<T> is delete[].
	// All allocations return a GpuSpan<T> whose DeviceAddress() is directly
	// usable in shaders via buffer device address.
	//
	// Two instances are the typical setup: one for vertex data, one for index
	// data (mirrors the separate-heap design of MeshArena).
	//
	// Thread safety: NOT thread-safe. All Alloc/Free calls must occur on the
	// same thread. Use external synchronization if concurrent access is required.
	// In AetherCore, GpuHeap is only used during asset loading (single-threaded).
	//
	// Usage:
	//   heap.Initialize(ctx, { .capacityBytes = 256 << 20 });
	//   GpuSpan<Mesh::Vertex> verts = heap.Alloc<Mesh::Vertex>(count);
	//   heap.Upload(verts, cpuData, device, queue, pool);  // synchronous, one-time
	//   // verts.DeviceAddress() is now ready for DrawInstanceData.vertexBufferAddr
	//   heap.Free(verts);
	class GpuHeap
	{
	public:
		struct Desc
		{
			gpu::DeviceSize capacityBytes = 256ull * 1024 * 1024;
			gpu::BufferUsage additionalUsage = gpu::BufferUsage::None;
			const char* debugName = nullptr;
		};

		void Initialize(const VulkanContext& ctx, Desc desc);
		void Shutdown();

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
			span.data = nullptr; // device-local, no CPU pointer
			span.address = m_buffer.GetDeviceAddress() + offset;
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
			const VkDeviceSize offset = span.address - m_buffer.GetDeviceAddress();
			FreeBytes(offset, span.ByteSize());
			span = {};
		}

		// Synchronous upload: creates a transient staging buffer, copies, submits, waits idle.
		// Intended for static load-time geometry. For streaming uploads use MeshUploadQueue instead.
		// Engine-side overload: opaque gpu::Device / gpu::Queue / gpu::CommandPool.
		template<typename T>
		void Upload(GpuSpan<T> dst, std::span<const T> src, gpu::Device device, gpu::Queue queue, gpu::CommandPool pool)
		{
			UploadBytes(dst.address, src.data(), static_cast<VkDeviceSize>(src.size()) * sizeof(T), static_cast<VkDevice>(device), static_cast<VkQueue>(queue), static_cast<VkCommandPool>(pool));
		}

		// Vulkan-internal overload: raw Vk* for callers that already have them.
		template<typename T>
		void Upload(GpuSpan<T> dst, std::span<const T> src, VkDevice device, VkQueue queue, VkCommandPool pool)
		{
			UploadBytes(dst.address, src.data(), static_cast<VkDeviceSize>(src.size()) * sizeof(T), device, queue, pool);
		}

		[[nodiscard]] VkBuffer GetBuffer() const
		{
			return m_buffer.Get();
		}

		[[nodiscard]] gpu::DeviceAddress GetBaseAddress() const
		{
			return m_buffer.GetDeviceAddress();
		}

		// Returns the byte offset of a span's start within this heap's buffer.
		// Use this when you need a VkDeviceSize offset for vkCmdBindIndexBuffer.
		template<typename T>
		[[nodiscard]] VkDeviceSize GetOffset(GpuSpan<T> span) const
		{
			return span.address - m_buffer.GetDeviceAddress();
		}

	private:
		static constexpr VkDeviceSize kInvalidOffset = ~0ull;

		struct FreeBlock
		{
			VkDeviceSize offset;
			VkDeviceSize size;
		};

		UniqueBuffer m_buffer;
		VmaAllocator m_allocatorRef = nullptr;
		VkDevice m_deviceRef = VK_NULL_HANDLE;
		std::vector<FreeBlock> m_freeList;

		VkDeviceSize AllocBytes(VkDeviceSize bytes);
		void FreeBytes(VkDeviceSize offset, VkDeviceSize bytes);
		void UploadBytes(gpu::DeviceAddress dstAddr, const void* src, VkDeviceSize bytes, VkDevice device, VkQueue queue, VkCommandPool pool);
	};
} // namespace aether
