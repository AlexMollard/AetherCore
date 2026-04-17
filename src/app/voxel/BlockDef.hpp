#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace voxel
{
	// Block type IDs.  0 = air (invisible).
	enum class BlockId : std::uint16_t
	{
		Air = 0,
		Dirt,
		Grass,
		Stone,
		Sand,
		Gravel,
		Wood,
		Leaves,
		Water,
		Snow,

		Count_
	};

	// Face indices (must match kFaceNormals in voxel_chunk.slang).
	enum class Face : std::uint32_t
	{
		PosX = 0, // right
		NegX = 1, // left
		PosY = 2, // top
		NegY = 3, // bottom
		PosZ = 4, // front
		NegZ = 5, // back
	};

	// Per-face UV rect into the block atlas.
	struct FaceUV
	{
		glm::vec2 uvMin{ 0.0f }; // top-left corner in [0,1] atlas space
		glm::vec2 uvMax{ 1.0f }; // bottom-right corner
	};

	// Complete definition of one block type.
	struct BlockDef
	{
		bool opaque = true; // false for air, water, leaves (partial transparency)

		// UV rects, indexed by Face.
		// A block with uniform faces can set all six to the same rect.
		std::array<FaceUV, 6> faces{};
	};
} // namespace voxel
