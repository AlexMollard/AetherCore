#pragma once

#include <cstdint>
#include <vector>
#include <vk_mem_alloc.h>
#include "volk.hpp"

#include "Mesh.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	class VulkanContext;

	// A large pre-allocated pair of device-local buffers (vertex + index) that voxel
	// chunk meshes suballocate from via a simple sorted free-list.
	//
	// Eliminates per-chunk VkBuffer create/destroy overhead and VMA fragmentation.
	// The arena works in raw bytes - callers supply byteSize alongside counts so the
	// arena stays vertex-format agnostic.
	//
	// Usage:
	//   1. Initialize(ctx)                     - once, at startup.
	//   2. alloc = Allocate(vBytes, vCount, iBytes, iCount)  - per chunk spawn.
	//   3. mesh  = CreateView(alloc)            - returns a non-owning Mesh* for DrawCommand.
	//   4. Free(alloc)                          - when the chunk is unloaded.
	//   5. Shutdown()                           - once, at teardown.
	class MeshArena
	{
	public:
		struct Desc
		{
			VkDeviceSize vertexCapacityBytes = 256ull * 1024 * 1024; // 256 MB vertex pool
			VkDeviceSize indexCapacityBytes = 128ull * 1024 * 1024;  // 128 MB index pool
		};

		struct Alloc
		{
			VkDeviceSize vertexByteOffset = 0;
			std::uint32_t vertexCount = 0;
			VkDeviceSize indexByteOffset = 0;
			std::uint32_t indexCount = 0;

			// Internal accounting - keep these to hand the bytes back to the free list.
			VkDeviceSize vertexByteSize = 0;
			VkDeviceSize indexByteSize = 0;

			[[nodiscard]] bool IsValid() const
			{
				return vertexByteSize > 0;
			}
		};

		void Initialize(const VulkanContext& ctx, const Desc& desc = {});
		void Shutdown();

		// Allocate a contiguous sub-region from both pools.
		// Returns an invalid Alloc ({}) when either pool is exhausted.
		[[nodiscard]] Alloc Allocate(VkDeviceSize vertexBytes, std::uint32_t vertexCount, VkDeviceSize indexBytes, std::uint32_t indexCount);

		// Return a sub-region to the free list and coalesce adjacent blocks.
		void Free(Alloc& alloc);

		// Create a non-owning Mesh view that points into the arena buffers at alloc's
		// offsets.  The view is invalidated the moment Free(alloc) is called.
		[[nodiscard]] Mesh CreateView(const Alloc& alloc) const;

		[[nodiscard]] VkBuffer GetVertexBuffer() const
		{
			return m_vertexBuffer.Get();
		}

		[[nodiscard]] VkBuffer GetIndexBuffer() const
		{
			return m_indexBuffer.Get();
		}

	private:
		struct FreeBlock
		{
			VkDeviceSize offset;
			VkDeviceSize size;
		};

		[[nodiscard]] VkDeviceSize AllocFromList(std::vector<FreeBlock>& list, VkDeviceSize size);
		void FreeToList(std::vector<FreeBlock>& list, VkDeviceSize offset, VkDeviceSize size);

		UniqueBuffer m_vertexBuffer;
		UniqueBuffer m_indexBuffer;
		std::vector<FreeBlock> m_vertexFreeList;
		std::vector<FreeBlock> m_indexFreeList;
	};
} // namespace aether
