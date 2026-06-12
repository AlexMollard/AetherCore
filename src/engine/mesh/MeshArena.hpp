#pragma once

#include <cstdint>
#include "gpu/GpuTypes.hpp"
#include "gpu/GpuHandles.hpp"

#include "vulkan/GpuHeap.hpp"
#include "mesh/Mesh.hpp"

namespace aether
{
	class VulkanContext;

	// A large pre-allocated pair of device-local buffers (vertex + index) that dynamic
	// meshes suballocate from via a sorted free-list.
	//
	// Backed by two GpuHeap instances so the free-list logic lives in one place.
	// The arena is vertex-format agnostic - callers supply byte sizes alongside counts.
	//
	// Usage:
	//   1. Initialize(ctx)                                            - once, at startup.
	//   2. alloc = Allocate(vBytes, vCount, iBytes, iCount)          - per chunk spawn.
	//   3. mesh  = CreateView(alloc)                                  - non-owning Mesh view.
	//   4. Free(alloc)                                                - when chunk is unloaded.
	//   5. Shutdown()                                                 - once, at teardown.
	class MeshArena
	{
	public:
		struct Desc
		{
			gpu::DeviceSize vertexCapacityBytes = 256ull * 1024 * 1024;
			gpu::DeviceSize indexCapacityBytes = 128ull * 1024 * 1024;
		};

		struct Alloc
		{
			gpu::DeviceSize vertexByteOffset = 0;
			std::uint32_t vertexCount = 0;
			gpu::DeviceSize indexByteOffset = 0;
			std::uint32_t indexCount = 0;

			// Internal accounting - keep these to hand the bytes back to the heap.
			gpu::DeviceSize vertexByteSize = 0;
			gpu::DeviceSize indexByteSize = 0;

			[[nodiscard]] bool IsValid() const
			{
				return vertexByteSize > 0;
			}
		};

		void Initialize(const VulkanContext& ctx, const Desc& desc);
		void Shutdown();

		// Allocate a contiguous sub-region from both pools.
		// Returns an invalid Alloc ({}) when either pool is exhausted.
		[[nodiscard]] Alloc Allocate(gpu::DeviceSize vertexBytes, std::uint32_t vertexCount, gpu::DeviceSize indexBytes, std::uint32_t indexCount);

		// Return a sub-region to the free list and coalesce adjacent blocks.
		void Free(Alloc& alloc);

		// Create a non-owning Mesh view that points into the arena buffers at alloc's offsets.
		// The view is invalidated the moment Free(alloc) is called.
		[[nodiscard]] Mesh CreateView(const Alloc& alloc) const;

		[[nodiscard]] gpu::BufferHandle GetVertexBuffer() const
		{
			return m_vertexHandle;
		}

		[[nodiscard]] gpu::BufferHandle GetIndexBuffer() const
		{
			return m_indexHandle;
		}

		[[nodiscard]] VkBuffer GetVertexBufferRaw() const
		{
			return m_vertexHeap.GetBuffer();
		}

		[[nodiscard]] VkBuffer GetIndexBufferRaw() const
		{
			return m_indexHeap.GetBuffer();
		}

		[[nodiscard]] gpu::DeviceAddress GetVertexDeviceAddress() const
		{
			return m_vertexHeap.GetBaseAddress();
		}

		[[nodiscard]] gpu::DeviceAddress GetIndexDeviceAddress() const
		{
			return m_indexHeap.GetBaseAddress();
		}

	private:
		GpuHeap m_vertexHeap;
		GpuHeap m_indexHeap;
		gpu::BufferHandle m_vertexHandle{};
		gpu::BufferHandle m_indexHandle{};
	};
} // namespace aether
