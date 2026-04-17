#include "MeshArena.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include "VulkanContext.hpp"

namespace aether
{
	void MeshArena::Initialize(const VulkanContext& ctx, const Desc& desc)
	{
		m_vertexBuffer = UniqueBuffer::CreateDeviceLocal(ctx.GetAllocator(), ctx.GetDevice().device, desc.vertexCapacityBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

		m_indexBuffer = UniqueBuffer::CreateDeviceLocal(ctx.GetAllocator(), ctx.GetDevice().device, desc.indexCapacityBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

		m_vertexFreeList.push_back({ 0, desc.vertexCapacityBytes });
		m_indexFreeList.push_back({ 0, desc.indexCapacityBytes });
	}

	void MeshArena::Shutdown()
	{
		m_vertexBuffer.Reset();
		m_indexBuffer.Reset();
		m_vertexFreeList.clear();
		m_indexFreeList.clear();
	}

	MeshArena::Alloc MeshArena::Allocate(VkDeviceSize vertexBytes, std::uint32_t vertexCount, VkDeviceSize indexBytes, std::uint32_t indexCount)
	{
		const VkDeviceSize vOffset = AllocFromList(m_vertexFreeList, vertexBytes);
		if (vOffset == ~0ull)
			return {};

		const VkDeviceSize iOffset = AllocFromList(m_indexFreeList, indexBytes);
		if (iOffset == ~0ull)
		{
			// Roll back vertex allocation.
			FreeToList(m_vertexFreeList, vOffset, vertexBytes);
			return {};
		}

		return Alloc{ vOffset, vertexCount, iOffset, indexCount, vertexBytes, indexBytes };
	}

	void MeshArena::Free(Alloc& alloc)
	{
		if (!alloc.IsValid())
			return;

		FreeToList(m_vertexFreeList, alloc.vertexByteOffset, alloc.vertexByteSize);
		FreeToList(m_indexFreeList, alloc.indexByteOffset, alloc.indexByteSize);
		alloc = {};
	}

	Mesh MeshArena::CreateView(const Alloc& alloc) const
	{
		return Mesh::CreateView(m_vertexBuffer.Get(), m_indexBuffer.Get(), alloc.vertexCount, alloc.indexCount, alloc.vertexByteOffset, alloc.indexByteOffset);
	}

	VkDeviceSize MeshArena::AllocFromList(std::vector<FreeBlock>& list, VkDeviceSize size)
	{
		for (auto it = list.begin(); it != list.end(); ++it)
		{
			if (it->size >= size)
			{
				const VkDeviceSize offset = it->offset;
				if (it->size == size)
					list.erase(it);
				else
				{
					it->offset += size;
					it->size -= size;
				}
				return offset;
			}
		}
		return ~0ull; // pool exhausted
	}

	void MeshArena::FreeToList(std::vector<FreeBlock>& list, VkDeviceSize offset, VkDeviceSize size)
	{
		// Insert sorted by offset for O(1) coalescing.
		auto it = std::lower_bound(list.begin(), list.end(), offset, [](const FreeBlock& b, VkDeviceSize o) { return b.offset < o; });
		it = list.insert(it, { offset, size });

		// Merge with the next block if adjacent.
		if (const auto next = std::next(it); next != list.end() && it->offset + it->size == next->offset)
		{
			it->size += next->size;
			list.erase(next);
		}

		// Merge with the previous block if adjacent.
		if (it != list.begin())
		{
			const auto prev = std::prev(it);
			if (prev->offset + prev->size == it->offset)
			{
				prev->size += it->size;
				list.erase(it);
			}
		}
	}
} // namespace aether
