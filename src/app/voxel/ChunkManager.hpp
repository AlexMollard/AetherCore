#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../DynamicMesh.hpp"
#include "BlockRegistry.hpp"
#include "ChunkMesher.hpp"

namespace aether
{
	class AetherCore;
	class GraphicsPipeline;
} // namespace aether

namespace voxel
{
	struct ChunkCoordHash
	{
		[[nodiscard]] std::size_t operator()(const glm::ivec3& value) const noexcept
		{
			std::size_t h = 0;
			auto hashCombine = [&h](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };

			hashCombine(std::hash<int>{}(value.x));
			hashCombine(std::hash<int>{}(value.y));
			hashCombine(std::hash<int>{}(value.z));
			return h;
		}
	};

	struct ChunkCoordEq
	{
		[[nodiscard]] bool operator()(const glm::ivec3& a, const glm::ivec3& b) const noexcept
		{
			return a.x == b.x && a.y == b.y && a.z == b.z;
		}
	};

	// Per-chunk runtime state.
	struct Chunk
	{
		// Raw block data in padded layout: (kPaddedSize)³ entries.
		// Index 0 = (-1,-1,-1) corner; visible voxels at [1, kChunkSize].
		std::vector<std::uint16_t> paddedBlocks;

		// Renderable GPU mesh — empty until first build.
		aether::DynamicMesh mesh;

		bool needsRebuild = true;
		bool isEmpty = false; // true once meshing confirms all-air
	};

	// ChunkManager — owns all loaded chunks and drives mesh rebuilds + draw submission.
	//
	// Usage (one-time setup):
	//   ChunkManager mgr;
	//   mgr.Initialize(core, registry, voxelPipeline);
	//   // populate chunks:
	//   mgr.SetBlock({0,0,0}, BlockId::Grass);
	//
	// Per-frame (inside the app update loop before AetherCore::BeginFrame):
	//   mgr.Update(playerPos);
	//   // Then in render:
	//   mgr.SubmitDraws(queue, frameIndex);
	class ChunkManager
	{
	public:
		struct DebugStats
		{
			std::size_t totalChunks = 0;
			std::size_t dirtyChunks = 0;
			std::size_t readyChunks = 0;
			std::size_t emptyChunks = 0;
			std::uint32_t submittedDrawsLastFrame = 0;
			std::uint32_t rebuildAttemptsLastFrame = 0;
			std::uint32_t rebuildUploadsLastFrame = 0;
			std::uint32_t rebuildFailuresLastFrame = 0;
		};

		ChunkManager() = default;

		// renderRadius: how many chunks around the player to keep loaded (Manhattan-ish distance in chunk units).
		void Initialize(aether::AetherCore& core, BlockRegistry& registry, const aether::GraphicsPipeline* pipeline, int renderRadius = 8);

		void Shutdown(aether::AetherCore& core);

		// Place or remove a single block at world integer coordinates.
		// Marks the affected chunk (and any bordering chunks) dirty.
		void SetBlock(const glm::ivec3& worldPos, BlockId id);

		// Query a single block at world integer coordinates.
		[[nodiscard]] BlockId GetBlock(const glm::ivec3& worldPos) const;

		// Rebuild dirty chunk meshes and upload to GPU.
		// Call every frame before submitting draws.
		void Update(const glm::vec3& playerWorldPos);

		// Submit all non-empty chunks to the RenderQueue.
		void SubmitDraws(aether::AetherCore& core);

		[[nodiscard]] DebugStats GetDebugStats() const;

	private:
		// Convert world block coord → chunk coord.
		static glm::ivec3 ChunkCoord(const glm::ivec3& worldPos);

		// Convert world block coord → local coord within chunk [0, kChunkSize).
		static glm::ivec3 LocalCoord(const glm::ivec3& worldPos);

		// Get or create a chunk, returning a pointer to it.
		Chunk& GetOrCreateChunk(const glm::ivec3& chunkCoord);

		// Write block into padded array of the owning chunk AND update the
		// one-voxel border of any adjacent chunks that see this position.
		void WriteBlockToChunks(const glm::ivec3& worldPos, BlockId id);

		// Maximum dirty chunks rebuilt (and GPU-uploaded) per Update() call.
		// Keeps the staging ring from overflowing during large world loads.
		static constexpr int kMaxUploadsPerFrame = 8;

		// Rebuild the mesh for a single chunk.
		// Returns false if the staging ring was full/failed — chunk stays dirty for next frame.
		bool RebuildChunk(const glm::ivec3& chunkCoord, Chunk& chunk);

		std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>, ChunkCoordHash, ChunkCoordEq> m_chunks;

		aether::AetherCore* m_core = nullptr;
		BlockRegistry* m_registry = nullptr;
		const aether::GraphicsPipeline* m_pipeline = nullptr;

		int m_renderRadius = 8;
		std::uint32_t m_submittedDrawsLastFrame = 0;
		std::uint32_t m_rebuildAttemptsLastFrame = 0;
		std::uint32_t m_rebuildUploadsLastFrame = 0;
		std::uint32_t m_rebuildFailuresLastFrame = 0;

		// Reusable mesher (avoids per-frame allocation).
		ChunkMesher m_mesher;
	};
} // namespace voxel
