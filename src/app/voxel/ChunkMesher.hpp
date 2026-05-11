#pragma once

#include <cstdint>
#include <vector>

#include "mesh/VoxelVertex.hpp"
#include "BlockDef.hpp"

namespace voxel
{
	class BlockRegistry;

	// Chunk storage constants.
	// Each chunk is CHUNK_SIZE³ blocks.  The mesher receives a padded 3-D array
	// of (CHUNK_SIZE + 2)³ blocks (one voxel border on all sides) so face-visibility
	// tests against neighbours never go out of bounds.
	inline constexpr int kChunkSize = 32;              // visible voxels per axis
	inline constexpr int kPaddedSize = kChunkSize + 2; // padded axis length

	// Linearisation helper for the padded volume.
	//   x, y, z in [0, kPaddedSize)
	inline constexpr int PaddedIndex(int x, int y, int z) noexcept
	{
		return x + kPaddedSize * (y + kPaddedSize * z);
	}

	// ChunkMesher - builds a simple face-culling mesh for a single chunk.
	//
	// Algorithm: for every voxel face that borders a transparent/air block, emit
	// two triangles.  Greedy merging may be added later; the current approach is
	// correct and fast enough for a first pass.
	//
	// Usage:
	//   ChunkMesher mesher;
	//   mesher.Build(paddedBlocks, registry, worldOrigin);
	//   DynamicMesh dm;
	//   dm.Rebuild(mesher.Vertices(), mesher.VertexCount(), ...);
	class ChunkMesher
	{
	public:
		ChunkMesher() = default;

		// Build the mesh.
		// blocks  : padded block array, size = kPaddedSize³, indexed with PaddedIndex().
		//           Index [0] is the (-1,-1,-1) corner; the visible chunk occupies
		//           x,y,z in [1, kChunkSize].
		// registry: provides face UV and opacity data.
		// origin  : world-space bottom-south-west corner of the visible chunk (not the padded region).
		void Build(const std::uint16_t* blocks, const BlockRegistry& registry, const glm::ivec3& origin);

		void Clear();

		[[nodiscard]] const aether::VoxelVertex* Vertices() const
		{
			return m_vertices.data();
		}

		[[nodiscard]] std::uint32_t VertexCount() const
		{
			return static_cast<std::uint32_t>(m_vertices.size());
		}

		[[nodiscard]] const std::uint32_t* Indices() const
		{
			return m_indices.data();
		}

		[[nodiscard]] std::uint32_t IndexCount() const
		{
			return static_cast<std::uint32_t>(m_indices.size());
		}

	private:
		void EmitFace(const glm::ivec3& voxelPos, Face face, const FaceUV& uv, std::uint8_t aoLevel);

		std::vector<aether::VoxelVertex> m_vertices;
		std::vector<std::uint32_t> m_indices;
	};
} // namespace voxel
