#include "ChunkMesher.hpp"

#include <glm/glm.hpp>

#include "mesh/VoxelVertex.hpp"
#include "utils/Profiler.hpp"
#include "BlockRegistry.hpp"

namespace voxel
{
	namespace
	{
		// Per-face vertex offsets (cube corner positions, +Y-up, +Z-forward right-hand).
		// Each row: 4 vertices for one face (CCW winding when viewed from outside).
		struct FaceGeom
		{
			glm::vec3 corners[4]; // local-space offsets from voxel origin (integer coords)
		};

		constexpr FaceGeom kFaceGeom[6] = {
			// PosX (+X face, right)
			{ { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } } },
			// NegX (-X face, left)
			{ { { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 } } },
			// PosY (+Y face, top)
			{ { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } } },
			// NegY (-Y face, bottom)
			{ { { 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 } } },
			// PosZ (+Z face, front)
			{ { { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 } } },
			// NegZ (-Z face, back)
			{ { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } } },
		};

		// Face-space UV corners for the 4 quad vertices (matches corner order above).
		constexpr glm::vec2 kFaceUVCorners[4] = {
			{ 0.0f, 1.0f }, // bottom-left
			{ 0.0f, 0.0f }, // top-left
			{ 1.0f, 0.0f }, // top-right
			{ 1.0f, 1.0f }, // bottom-right
		};

		// Quad index pattern: two CCW triangles.
		constexpr std::uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };

		// Neighbour offsets per face for visibility check.
		constexpr glm::ivec3 kNeighbour[6] = {
			{  1,  0,  0 },
			{ -1,  0,  0 },
			{  0,  1,  0 },
			{  0, -1,  0 },
			{  0,  0,  1 },
			{  0,  0, -1 },
		};
	} // namespace

	void ChunkMesher::Build(const std::uint16_t* blocks, const BlockRegistry& registry, const glm::ivec3& origin)
	{
		AE_PROFILE_ZONE();
		Clear();

		// Estimate: average chunk won't fill more than ~30% of faces.
		m_vertices.reserve(kChunkSize * kChunkSize * kChunkSize / 4);
		m_indices.reserve(m_vertices.capacity() / 4 * 6);

		// Iterate over visible voxels (padded coords [1, kChunkSize]).
		for (int z = 1; z <= kChunkSize; ++z)
		{
			for (int y = 1; y <= kChunkSize; ++y)
			{
				for (int x = 1; x <= kChunkSize; ++x)
				{
					const std::uint16_t rawId = blocks[PaddedIndex(x, y, z)];
					if (rawId == 0u)
					{
						continue; // air
					}

					const BlockId blockId = static_cast<BlockId>(rawId);
					if (!registry.IsOpaque(blockId))
					{
						continue; // skip transparent / partial blocks (handled separately later)
					}

					const BlockDef& def = registry.Get(blockId);

					// Visible-chunk world position of this voxel.
					const glm::ivec3 voxelWorld = origin + glm::ivec3(x - 1, y - 1, z - 1);

					for (std::uint32_t fi = 0; fi < 6; ++fi)
					{
						const Face face = static_cast<Face>(fi);
						const glm::ivec3 nbPos = glm::ivec3(x, y, z) + kNeighbour[fi];

						// Fetch neighbour from the padded array (always in range).
						const std::uint16_t nbRaw = blocks[PaddedIndex(nbPos.x, nbPos.y, nbPos.z)];
						const BlockId nbId = static_cast<BlockId>(nbRaw);

						// Only emit this face if the neighbour is non-opaque.
						if (nbRaw != 0u && registry.IsOpaque(nbId))
						{
							continue;
						}

						EmitFace(voxelWorld, face, def.faces[fi], 0u /* aoLevel unused for now */);
					}
				}
			}
		}
	}

	void ChunkMesher::Clear()
	{
		m_vertices.clear();
		m_indices.clear();
	}

	void ChunkMesher::EmitFace(const glm::ivec3& voxelPos, Face face, const FaceUV& uv, std::uint8_t aoLevel)
	{
		const FaceGeom& geom = kFaceGeom[static_cast<std::uint32_t>(face)];
		const std::uint32_t fi = static_cast<std::uint32_t>(face);

		const std::uint32_t baseVertex = static_cast<std::uint32_t>(m_vertices.size());

		for (int v = 0; v < 4; ++v)
		{
			aether::VoxelVertex vtx{};
			vtx.position = glm::vec3(voxelPos) + geom.corners[v];

			// Interpolate UV within the atlas rect.
			const glm::vec2 t = kFaceUVCorners[v];
			vtx.uv = glm::mix(uv.uvMin, uv.uvMax, t);

			vtx.packed = aether::VoxelVertex::PackFace(fi, aoLevel);
			m_vertices.push_back(vtx);
		}

		for (std::uint32_t i = 0; i < 6; ++i)
		{
			m_indices.push_back(baseVertex + kQuadIndices[i]);
		}
	}
} // namespace voxel
