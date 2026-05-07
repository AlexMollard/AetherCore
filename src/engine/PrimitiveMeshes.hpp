#pragma once

#include <vk_mem_alloc.h>
#include "volk.hpp"

#include "Mesh.hpp"

namespace aether
{
	enum class PrimitiveMesh
	{
		Triangle,
		Quad,
		Cube,
		Plane,  // 20×20 subdivided grid; UVs tile 20× per axis via REPEAT sampler
		Sphere, // UV sphere, radius 0.5, 32 slices × 16 stacks
	};

	// Engine-owned cache of built-in primitive meshes.
	class PrimitiveMeshes
	{
	public:
		void Initialize(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool);
		[[nodiscard]] const Mesh& Get(PrimitiveMesh primitive) const;

	private:
		Mesh m_triangle;
		Mesh m_quad;
		Mesh m_cube;
		Mesh m_plane;
		Mesh m_sphere;
	};
} // namespace aether
