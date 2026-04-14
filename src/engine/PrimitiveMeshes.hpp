#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "Mesh.hpp"

namespace aether
{
	enum class PrimitiveMesh
	{
		Triangle,
		Quad,
		Cube,
	};

	// Engine-owned cache of built-in primitive meshes.
	class PrimitiveMeshes
	{
	public:
		void Initialize(VkDevice device, VmaAllocator allocator);
		[[nodiscard]] const Mesh& Get(PrimitiveMesh primitive) const;

	private:
		Mesh m_triangle;
		Mesh m_quad;
		Mesh m_cube;
	};
}
