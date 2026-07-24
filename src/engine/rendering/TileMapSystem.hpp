#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/Entity.hpp"
#include "utils/Hash.hpp"

namespace aether
{
	class SpriteAssetStore;
	class TileAssetStore;
	class TextureRegistry;
	class World;

	// Camera-visible world rect on the XY plane; invalid = no culling.
	struct View2DBounds
	{
		glm::vec2 min{0.0f};
		glm::vec2 max{0.0f};
		bool valid = false;
	};

	// Extracts visible tile chunks into sprite-compatible instances appended to
	// Render2DFrameData::sprites (the shared Finalize2DFrame sort interleaves
	// them with sprites). One instance array is cached per (entity, layer,
	// chunk), keyed by the chunk's revision and the entity transform, so only
	// edited chunks rebuild. Animated tiles patch UVs during the copy - the
	// cache itself never rebuilds for animation.
	class TileMapSystem
	{
	public:
		void Initialize(TextureRegistry& textures, SpriteAssetStore& sprites, TileAssetStore& tiles);
		void Shutdown();

		void Extract(World& world, const View2DBounds& view, float elapsedSeconds, Render2DFrameData& output);

		void InvalidateAll();

		// Diagnostics/tests: chunk cache rebuilds since construction.
		[[nodiscard]] std::uint64_t RebuildCount() const noexcept
		{
			return m_rebuildCount;
		}

	private:
		struct ChunkCacheKey
		{
			std::uint32_t entity = 0;
			std::uint32_t layer = 0;
			std::int32_t chunkX = 0;
			std::int32_t chunkY = 0;
			bool operator==(const ChunkCacheKey&) const = default;
		};
		struct ChunkCacheKeyHash
		{
			[[nodiscard]] std::size_t operator()(const ChunkCacheKey& key) const noexcept
			{
				std::size_t hash = std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(key.entity) << 32u) | key.layer);
				hash = utils::HashCombine(hash, std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.chunkX)) << 32u) | static_cast<std::uint32_t>(key.chunkY)));
				return hash;
			}
		};
		struct AnimatedSlot
		{
			std::uint32_t instanceIndex = 0; // within the cached array
			std::uint32_t tileIndex = 0;     // into the tileset's tiles
		};
		struct ChunkCache
		{
			std::vector<SpriteRenderInstance> instances;
			std::vector<AnimatedSlot> animated;
			// Solid tile cells as shadow occluders (xy = world centre, z = half cell).
			std::vector<glm::vec4> occluders;
			std::uint32_t builtRevision = 0;
			glm::mat4 builtTransform{1.0f};
			std::uint64_t lastTouchedFrame = 0;
		};

		[[nodiscard]] TextureHandle ResolveTexture(const std::string& path);

		TextureRegistry* m_textures = nullptr;
		SpriteAssetStore* m_sprites = nullptr;
		TileAssetStore* m_tiles = nullptr;
		std::unordered_map<ChunkCacheKey, ChunkCache, ChunkCacheKeyHash> m_chunks;
		std::unordered_map<std::string, TextureHandle> m_textureCache;
		std::uint64_t m_rebuildCount = 0;
		std::uint64_t m_extractCount = 0;
	};
} // namespace aether
