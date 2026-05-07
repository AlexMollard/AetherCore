#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// Compact per-vertex layout for GPU-driven voxel chunk meshes.
	// 24 bytes vs 92 bytes for Mesh::Vertex.
	//
	//   offset  0 : float3  position  (12) - world-space vertex position
	//   offset 12 : uint32  packed     (4) - bits[0-2]=faceIndex(0-5), bits[3-4]=aoLevel(0-3)
	//   offset 16 : float2  uv         (8) - atlas UV coordinates
	//   Total: 24 bytes
	struct VoxelVertex
	{
		glm::vec3 position;   // 12 bytes
		std::uint32_t packed; //  4 bytes
		glm::vec2 uv;         //  8 bytes

		static constexpr std::uint32_t PackFace(std::uint32_t faceIndex, std::uint32_t aoLevel)
		{
			return (faceIndex & 0x7u) | ((aoLevel & 0x3u) << 3);
		}
	};

	static_assert(sizeof(VoxelVertex) == 24, "VoxelVertex size changed - update GraphicsPipeline voxel input state.");
} // namespace aether
