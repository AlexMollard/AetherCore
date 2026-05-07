#include "DynamicMesh.hpp"

#include <cassert>

#include "MeshUploadQueue.hpp"

namespace aether
{
	DynamicMesh::~DynamicMesh()
	{
		FreeAlloc();
	}

	DynamicMesh::DynamicMesh(DynamicMesh&& other) noexcept
	      : m_alloc(other.m_alloc), m_mesh(std::move(other.m_mesh)), m_arena(other.m_arena)
	{
		other.m_alloc = {};
		other.m_arena = nullptr;
	}

	DynamicMesh& DynamicMesh::operator=(DynamicMesh&& other) noexcept
	{
		if (this != &other)
		{
			FreeAlloc();
			m_alloc = other.m_alloc;
			m_mesh = std::move(other.m_mesh);
			m_arena = other.m_arena;

			other.m_alloc = {};
			other.m_arena = nullptr;
		}
		return *this;
	}

	bool DynamicMesh::Rebuild(const void* vertexData, std::uint32_t vertexCount, std::uint32_t vertexStride, const std::uint32_t* indices, std::uint32_t indexCount, MeshArena& arena, MeshUploadQueue& uploadQueue)
	{
		// Release the old allocation back to the arena.
		FreeAlloc();
		m_arena = &arena;

		const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * vertexStride;
		const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) * sizeof(std::uint32_t);

		m_alloc = arena.Allocate(vertexBytes, vertexCount, indexBytes, indexCount);
		if (!m_alloc.IsValid())
		{
			// Arena exhausted this frame; caller should retry later after capacity
			// is increased or other chunks are unloaded.
			return false;
		}

		const bool queued = uploadQueue.Upload(vertexData, vertexBytes, arena.GetVertexBuffer(), m_alloc.vertexByteOffset, indices, indexBytes, arena.GetIndexBuffer(), m_alloc.indexByteOffset);
		if (!queued)
		{
			// Staging ring full this frame - release the arena slot immediately so
			// no stale / uninitialised geometry is submitted, and signal the caller
			// to retry next frame.
			arena.Free(m_alloc);
			m_alloc = {};
			return false;
		}

		m_mesh = arena.CreateView(m_alloc);
		return true;
	}

	void DynamicMesh::Reset(MeshArena& arena)
	{
		(void) arena;
		FreeAlloc();
		m_alloc = {};
		m_mesh = Mesh{};
		m_arena = nullptr;
	}

	void DynamicMesh::FreeAlloc()
	{
		if (m_arena && m_alloc.IsValid())
		{
			m_arena->Free(m_alloc);
			m_alloc = {};
		}
	}
} // namespace aether
