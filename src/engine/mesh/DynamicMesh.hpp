#pragma once

#include <cstdint>

#include "mesh/Mesh.hpp"
#include "mesh/MeshArena.hpp"

namespace aether
{
	class MeshUploadQueue;

	class DynamicMesh
	{
	public:
		DynamicMesh() = default;
		~DynamicMesh();

		DynamicMesh(const DynamicMesh&) = delete;
		DynamicMesh& operator=(const DynamicMesh&) = delete;

		DynamicMesh(DynamicMesh&&) noexcept;
		DynamicMesh& operator=(DynamicMesh&&) noexcept;

		[[nodiscard]] bool Rebuild(const void* vertexData, std::uint32_t vertexCount, std::uint32_t vertexStride, const std::uint32_t* indices, std::uint32_t indexCount, MeshArena& arena, MeshUploadQueue& uploadQueue);

		void Reset();

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
		MeshArena* m_arena = nullptr;
	};
} // namespace aether
