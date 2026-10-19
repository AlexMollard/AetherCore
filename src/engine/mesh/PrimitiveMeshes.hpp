#pragma once

#include "gpu/UploadContext.hpp"

#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMesh.hpp"

namespace aether
{

	class PrimitiveMeshes
	{
	public:
		void Initialize(gpu::UploadContext& uploadContext);
		void Destroy();
		[[nodiscard]] const Mesh& Get(PrimitiveMesh primitive) const;

	private:
		Mesh m_triangle;
		Mesh m_quad;
		Mesh m_cube;
		Mesh m_plane;
		Mesh m_sphere;
	};
} // namespace aether
