#include "ChunkManager.hpp"

#include <algorithm>
#include <cassert>

#include "../AetherCore.hpp"
#include "../GraphicsPipeline.hpp"
#include "../RenderQueue.hpp"

namespace voxel
{
	namespace
	{
		// Returns the chunk coordinate that owns the given world block position.
		// Chunk coords are always floored (so negative world coords map correctly).
		glm::ivec3 WorldToChunk(const glm::ivec3& worldPos)
		{
			// Integer floor-divide.
			auto fd = [](int v, int d) -> int { return (v / d) - (v % d != 0 && (v ^ d) < 0 ? 1 : 0); };
			return { fd(worldPos.x, kChunkSize), fd(worldPos.y, kChunkSize), fd(worldPos.z, kChunkSize) };
		}

		// Returns position of the chunk's (0,0,0) voxel in world space.
		glm::ivec3 ChunkOrigin(const glm::ivec3& chunkCoord)
		{
			return chunkCoord * kChunkSize;
		}

		// Convert world position to local [0, kChunkSize) coordinate.
		glm::ivec3 WorldToLocal(const glm::ivec3& worldPos, const glm::ivec3& chunkCoord)
		{
			const glm::ivec3 origin = ChunkOrigin(chunkCoord);
			return worldPos - origin;
		}

		// Padded index for a local coord (local in [0, kChunkSize)).
		// Add 1 to account for the one-voxel ghost border.
		int LocalToPaddedIndex(const glm::ivec3& local)
		{
			return PaddedIndex(local.x + 1, local.y + 1, local.z + 1);
		}
	} // namespace

	// ── Lifecycle ─────────────────────────────────────────────────────────────

	void ChunkManager::Initialize(aether::AetherCore& core, BlockRegistry& registry, const aether::GraphicsPipeline* pipeline, int renderRadius)
	{
		m_core = &core;
		m_registry = &registry;
		m_pipeline = pipeline;
		m_renderRadius = renderRadius;
	}

	void ChunkManager::Shutdown(aether::AetherCore& core)
	{
		for (auto& [coord, chunk]: m_chunks)
		{
			if (chunk)
				chunk->mesh.Reset(core.GetMeshArena());
		}
		m_chunks.clear();
	}

	// ── Block access ──────────────────────────────────────────────────────────

	glm::ivec3 ChunkManager::ChunkCoord(const glm::ivec3& worldPos)
	{
		return WorldToChunk(worldPos);
	}

	glm::ivec3 ChunkManager::LocalCoord(const glm::ivec3& worldPos)
	{
		const glm::ivec3 cc = ChunkCoord(worldPos);
		return WorldToLocal(worldPos, cc);
	}

	Chunk& ChunkManager::GetOrCreateChunk(const glm::ivec3& chunkCoord)
	{
		auto it = m_chunks.find(chunkCoord);
		if (it != m_chunks.end())
			return *it->second;

		auto& ptr = m_chunks.emplace(chunkCoord, std::make_unique<Chunk>()).first->second;
		ptr->paddedBlocks.assign(kPaddedSize * kPaddedSize * kPaddedSize, 0u);
		return *ptr;
	}

	void ChunkManager::WriteBlockToChunks(const glm::ivec3& worldPos, BlockId id)
	{
		const glm::ivec3 cc = ChunkCoord(worldPos);
		const glm::ivec3 local = WorldToLocal(worldPos, cc);

		// Write into owning chunk.
		Chunk& owningChunk = GetOrCreateChunk(cc);
		owningChunk.paddedBlocks[LocalToPaddedIndex(local)] = static_cast<std::uint16_t>(id);
		owningChunk.needsRebuild = true;

		// Update ghost border cells of adjacent chunks.
		for (int fi = 0; fi < 6; ++fi)
		{
			// Is this voxel on the face boundary?
			const int axis = fi >> 1; // 0=X, 1=Y, 2=Z
			const int sign = (fi & 1) ? -1 : 1;
			const int localVal = local[axis];
			const int boundary = (sign > 0) ? (kChunkSize - 1) : 0;
			if (localVal != boundary)
				continue;

			// Determine neighbour chunk.
			glm::ivec3 nbCC = cc;
			nbCC[axis] += sign;

			// Only update if neighbour already exists (don't force-create).
			auto it = m_chunks.find(nbCC);
			if (it == m_chunks.end())
				continue;

			Chunk& nb = *it->second;
			// Ghost position in neighbour padded space:
			// the ghost border is at padded coord 0 or kPaddedSize-1.
			glm::ivec3 ghostPadded = glm::ivec3(local.x + 1, local.y + 1, local.z + 1);
			ghostPadded[axis] = (sign > 0) ? 0 : (kPaddedSize - 1);
			nb.paddedBlocks[PaddedIndex(ghostPadded.x, ghostPadded.y, ghostPadded.z)] = static_cast<std::uint16_t>(id);
			nb.needsRebuild = true;
		}
	}

	void ChunkManager::SetBlock(const glm::ivec3& worldPos, BlockId id)
	{
		WriteBlockToChunks(worldPos, id);
	}

	BlockId ChunkManager::GetBlock(const glm::ivec3& worldPos) const
	{
		const glm::ivec3 cc = ChunkCoord(worldPos);
		const glm::ivec3 local = WorldToLocal(worldPos, cc);

		auto it = m_chunks.find(cc);
		if (it == m_chunks.end())
			return BlockId::Air;

		const Chunk& chunk = *it->second;
		return static_cast<BlockId>(chunk.paddedBlocks[LocalToPaddedIndex(local)]);
	}

	// ── Per-frame update ──────────────────────────────────────────────────────

	bool ChunkManager::RebuildChunk(const glm::ivec3& chunkCoord, Chunk& chunk)
	{
		m_mesher.Build(chunk.paddedBlocks.data(), *m_registry, ChunkOrigin(chunkCoord));

		if (m_mesher.VertexCount() == 0)
		{
			chunk.isEmpty = true;
			chunk.mesh.Reset(m_core->GetMeshArena());
			chunk.needsRebuild = false;
			return true;
		}

		chunk.isEmpty = false;
		const bool uploaded = chunk.mesh.Rebuild(m_mesher.Vertices(), m_mesher.VertexCount(), sizeof(aether::VoxelVertex), m_mesher.Indices(), m_mesher.IndexCount(), m_core->GetMeshArena(), m_core->GetMeshUploadQueue());

		if (uploaded)
			chunk.needsRebuild = false;
		// else: leave needsRebuild = true so we retry next frame

		return uploaded;
	}

	void ChunkManager::Update(const glm::vec3& playerWorldPos)
	{
		(void) playerWorldPos;
		assert(m_core && m_registry && m_pipeline);
		m_rebuildAttemptsLastFrame = 0;
		m_rebuildUploadsLastFrame = 0;
		m_rebuildFailuresLastFrame = 0;

		// Rebuild up to kMaxUploadsPerFrame dirty chunks this frame, then flush.
		// Remaining dirty chunks are deferred to the next frame, which naturally
		// spreads large initial terrain loads across multiple frames without ever
		// overflowing the staging ring.
		int uploadsThisFrame = 0;
		int attemptsThisFrame = 0;
		constexpr int kMaxAttemptsPerFrame = kMaxUploadsPerFrame * 8;
		for (auto& [coord, chunk]: m_chunks)
		{
			if (!chunk || !chunk->needsRebuild)
				continue;

			if (attemptsThisFrame >= kMaxAttemptsPerFrame)
				break;

			if (uploadsThisFrame >= kMaxUploadsPerFrame)
				break;

			++attemptsThisFrame;
			++m_rebuildAttemptsLastFrame;
			if (RebuildChunk(coord, *chunk))
			{
				++uploadsThisFrame;
				++m_rebuildUploadsLastFrame;
			}
			else
			{
				++m_rebuildFailuresLastFrame;
			}
			// On failure we keep scanning other dirty chunks to avoid one chunk
			// permanently starving all later entries in unordered_map iteration.
		}

		if (m_core->GetMeshUploadQueue().HasPendingUploads())
			m_core->FlushMeshUploads();
	}

	void ChunkManager::SubmitDraws(aether::AetherCore& core)
	{
		assert(m_pipeline);
		m_submittedDrawsLastFrame = 0;

		for (const auto& [coord, chunk]: m_chunks)
		{
			if (!chunk || chunk->isEmpty || chunk->needsRebuild)
				continue;

			const aether::Mesh& mesh = chunk->mesh.GetMesh();

			aether::DrawCommand cmd{};
			cmd.pipeline = m_pipeline;
			cmd.mesh = &mesh;
			cmd.materialIndex = m_registry->GetAtlasSlot();
			cmd.modelMatrix = glm::mat4(1.0f); // vertices already in world space
			cmd.worldBoundingSphere = glm::vec4(glm::vec3(ChunkOrigin(coord)) + glm::vec3(kChunkSize * 0.5f), glm::length(glm::vec3(kChunkSize * 0.5f)));

			core.GetRenderQueue().Submit(cmd);
			++m_submittedDrawsLastFrame;
		}
	}

	ChunkManager::DebugStats ChunkManager::GetDebugStats() const
	{
		DebugStats stats{};
		stats.totalChunks = m_chunks.size();
		stats.submittedDrawsLastFrame = m_submittedDrawsLastFrame;
		stats.rebuildAttemptsLastFrame = m_rebuildAttemptsLastFrame;
		stats.rebuildUploadsLastFrame = m_rebuildUploadsLastFrame;
		stats.rebuildFailuresLastFrame = m_rebuildFailuresLastFrame;

		for (const auto& [coord, chunk]: m_chunks)
		{
			(void) coord;
			if (!chunk)
				continue;

			if (chunk->needsRebuild)
				++stats.dirtyChunks;
			if (chunk->isEmpty)
				++stats.emptyChunks;
			if (!chunk->needsRebuild && !chunk->isEmpty && chunk->mesh.IsValid())
				++stats.readyChunks;
		}

		return stats;
	}
} // namespace voxel
