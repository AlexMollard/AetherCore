#include "mesh/DynamicMesh.hpp"

#include <cassert>

#include "mesh/MeshUploadQueue.hpp"
#include "utils/Profiler.hpp"

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
		AE_PROFILE_ZONE();
		FreeAlloc();
		m_arena = &arena;

		const gpu::DeviceSize vertexBytes = static_cast<gpu::DeviceSize>(vertexCount) * vertexStride;
		const gpu::DeviceSize indexBytes = static_cast<gpu::DeviceSize>(indexCount) * sizeof(std::uint32_t);

		m_alloc = arena.Allocate(vertexBytes, vertexCount, indexBytes, indexCount);
		if (!m_alloc.IsValid())
		{
			return false;
		}

		const bool queued = uploadQueue.Upload(vertexData, vertexBytes, arena.GetVertexBufferRaw(), m_alloc.vertexByteOffset, indices, indexBytes, arena.GetIndexBufferRaw(), m_alloc.indexByteOffset);
		if (!queued)
		{
			arena.Free(m_alloc);
			m_alloc = {};
			return false;
		}

		m_mesh = arena.CreateView(m_alloc);
		return true;
	}

	void DynamicMesh::Reset()
	{
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
