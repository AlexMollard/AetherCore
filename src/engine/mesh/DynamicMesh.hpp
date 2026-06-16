#pragma once

#include <cstdint>

#include "mesh/Mesh.hpp"
#include "mesh/MeshArena.hpp"

namespace aether
{
	class MeshUploadQueue;

	// A chunk mesh that can be rebuilt without allocating a new buffer.
	// Suballocates from a MeshArena and schedules the upload via MeshUploadQueue.
	//
	// Usage:
	//   DynamicMesh m;
	//   m.Rebuild(vertices.data(), vertCount, sizeof(MyVertex),
	//             indices.data(),  idxCount,
	//             arena, uploadQueue);
	//
	//   // Each frame:
	//   DrawCommand dc;
	//   dc.mesh = &m.GetMesh();
	//   renderQueue.Submit(dc);
	//
	//   // On block change:
	//   m.Rebuild(...);   // old arena allocation is freed automatically
	//
	//   // On chunk unload:
	//   m.Reset(arena);   // frees the arena allocation; the arena outlives the mesh
	class DynamicMesh
	{
	public:
		DynamicMesh() = default;
		~DynamicMesh();

		DynamicMesh(const DynamicMesh&) = delete;
		DynamicMesh& operator=(const DynamicMesh&) = delete;

		DynamicMesh(DynamicMesh&&) noexcept;
		DynamicMesh& operator=(DynamicMesh&&) noexcept;

		// Upload new geometry into the arena.  Frees the previous allocation first.
		// vertexStride = sizeof(your vertex type), e.g. sizeof(MyVertex).
		// Returns false if the staging ring is full - the old mesh is freed and the
		// chunk should be retried next frame (needsRebuild stays true at call site).
		[[nodiscard]] bool Rebuild(const void* vertexData, std::uint32_t vertexCount, std::uint32_t vertexStride, const std::uint32_t* indices, std::uint32_t indexCount, MeshArena& arena, MeshUploadQueue& uploadQueue);

		// Release the current arena allocation without rebuilding.
		// Safe to call on an already-empty DynamicMesh.
		void Reset(MeshArena& arena);

		[[nodiscard]] bool IsValid() const
		{
			return m_mesh.IsValid();
		}

		[[nodiscard]] const Mesh& GetMesh() const
		{
			return m_mesh;
		}

	private:
		void FreeAlloc();

		MeshArena::Alloc m_alloc{};
		Mesh m_mesh;
		MeshArena* m_arena = nullptr; // non-owning back-pointer for destructor
	};
} // namespace aether
