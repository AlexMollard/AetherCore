#include "rendering/TileMapSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		constexpr std::uint32_t kInvalidTextureSlot = 0xFFFFFFFFu;

		[[nodiscard]] std::uint32_t BiasSigned(std::int32_t value) noexcept
		{
			return static_cast<std::uint32_t>(std::clamp(value, -32768, 32767) + 32768);
		}

		[[nodiscard]] std::uint64_t MakeTileSortKey(std::int32_t sortingLayer, std::int32_t orderInLayer, std::uint32_t entityId) noexcept
		{
			const std::uint64_t layer = static_cast<std::uint64_t>(BiasSigned(sortingLayer));
			const std::uint64_t order = static_cast<std::uint64_t>(BiasSigned(orderInLayer));
			return (layer << 48u) | (order << 32u) | static_cast<std::uint64_t>(entityId);
		}

		// Same texel-centre inset the sprite path applies so tile edges never
		// bleed neighbouring atlas frames under linear filtering.
		void InsetUvRange(float& begin, float& end, float halfTexel) noexcept
		{
			const float midpoint = (begin + end) * 0.5f;
			const float direction = end >= begin ? 1.0f : -1.0f;
			const float halfSpan = std::max(std::abs(end - begin) * 0.5f - halfTexel, 0.0f);
			begin = midpoint - direction * halfSpan;
			end = midpoint + direction * halfSpan;
		}

		[[nodiscard]] glm::vec4 InsetAtlasUvRect(glm::vec4 uvRect, std::int32_t textureWidth, std::int32_t textureHeight) noexcept
		{
			if (textureWidth <= 0 || textureHeight <= 0)
			{
				return uvRect;
			}
			InsetUvRange(uvRect.x, uvRect.z, 0.5f / static_cast<float>(textureWidth));
			InsetUvRange(uvRect.y, uvRect.w, 0.5f / static_cast<float>(textureHeight));
			return uvRect;
		}
	} // namespace

	void TileMapSystem::Initialize(TextureRegistry& textures, SpriteAssetStore& sprites, TileAssetStore& tiles)
	{
		m_textures = &textures;
		m_sprites = &sprites;
		m_tiles = &tiles;
	}

	void TileMapSystem::Shutdown()
	{
		m_chunks.clear();
		m_textureCache.clear();
		m_textures = nullptr;
		m_sprites = nullptr;
		m_tiles = nullptr;
	}

	void TileMapSystem::InvalidateAll()
	{
		m_chunks.clear();
		m_textureCache.clear();
	}

	TextureHandle TileMapSystem::ResolveTexture(const std::string& path)
	{
		if (const auto it = m_textureCache.find(path); it != m_textureCache.end())
		{
			return it->second;
		}
		const TextureHandle handle = m_textures != nullptr ? m_textures->Acquire(path) : TextureHandle{};
		m_textureCache.emplace(path, handle);
		return handle;
	}

	void TileMapSystem::Extract(World& world, const View2DBounds& view, float elapsedSeconds, Render2DFrameData& output)
	{
		AE_PROFILE_ZONE_N("TileMap.Extract");
		if (m_tiles == nullptr || m_sprites == nullptr)
		{
			return;
		}
		++m_extractCount;

		auto entities = world.GetRegistry().view<const TransformComponent, const TileMapComponent>(entt::exclude<DisabledComponent>);
		for (const entt::entity raw: entities)
		{
			const auto& component = entities.get<const TileMapComponent>(raw);
			if (!component.visible || component.tilemapPath.empty())
			{
				continue;
			}
			const auto mapResult = m_tiles->LoadTileMap(component.tilemapPath);
			if (!mapResult.has_value())
			{
				AE_WARN(LogCategory::Engine, "Tile map '{}' failed to load: {}", component.tilemapPath, mapResult.error().ToString());
				continue;
			}
			const TileMapAsset& map = **mapResult;
			const auto tileSetResult = m_tiles->LoadTileSet(map.tileSetPath);
			if (!tileSetResult.has_value())
			{
				AE_WARN(LogCategory::Engine, "Tile set '{}' failed to load: {}", map.tileSetPath, tileSetResult.error().ToString());
				continue;
			}
			const TileSetAsset& tileSet = **tileSetResult;
			const float cellSize = map.cellSize > 0.0f ? map.cellSize : tileSet.cellSize;
			const Entity entity = World::FromEntt(raw);
			const glm::mat4& entityTransform = entities.get<const TransformComponent>(raw).localToWorld;

			for (std::size_t layerIndex = 0; layerIndex < map.layers.size(); ++layerIndex)
			{
				const TileMapLayer& layer = map.layers[layerIndex];
				if (!layer.visible || (layerIndex < 32 && (component.visibleLayerMask & (1u << layerIndex)) == 0u))
				{
					continue;
				}
				for (const auto& [chunkKey, chunk]: layer.chunks)
				{
					// Conservative world AABB of the chunk (handles rotation).
					if (view.valid)
					{
						const glm::vec2 localMin{static_cast<float>(chunkKey.x * kTileChunkSize) * cellSize, static_cast<float>(chunkKey.y * kTileChunkSize) * cellSize};
						const glm::vec2 localMax = localMin + glm::vec2{static_cast<float>(kTileChunkSize) * cellSize};
						glm::vec2 worldMin{std::numeric_limits<float>::max()};
						glm::vec2 worldMax{std::numeric_limits<float>::lowest()};
						for (const glm::vec2 corner: {localMin, glm::vec2{localMax.x, localMin.y}, localMax, glm::vec2{localMin.x, localMax.y}})
						{
							const glm::vec4 world4 = entityTransform * glm::vec4(corner, 0.0f, 1.0f);
							worldMin = glm::min(worldMin, glm::vec2(world4));
							worldMax = glm::max(worldMax, glm::vec2(world4));
						}
						if (worldMax.x < view.min.x || worldMin.x > view.max.x || worldMax.y < view.min.y || worldMin.y > view.max.y)
						{
							continue;
						}
					}

					const ChunkCacheKey cacheKey{entity.id, static_cast<std::uint32_t>(layerIndex), chunkKey.x, chunkKey.y};
					ChunkCache& cache = m_chunks[cacheKey];
					cache.lastTouchedFrame = m_extractCount;
					if (cache.builtRevision != chunk.revision || cache.builtTransform != entityTransform)
					{
						++m_rebuildCount;
						cache.instances.clear();
						cache.animated.clear();
						cache.occluders.clear();
						cache.builtRevision = chunk.revision;
						cache.builtTransform = entityTransform;

						const std::uint64_t sortKey = MakeTileSortKey(layer.sortingLayer + component.sortingLayer, layer.orderInLayer + component.orderInLayer, entity.id);
						const glm::vec4 color = component.tint * layer.tint * glm::vec4{1.0f, 1.0f, 1.0f, layer.opacity};

						for (std::int32_t localY = 0; localY < kTileChunkSize; ++localY)
						{
							for (std::int32_t localX = 0; localX < kTileChunkSize; ++localX)
							{
								const std::uint32_t cell = chunk.cells[static_cast<std::size_t>(localY) * kTileChunkSize + localX];
								if (tilecell::Empty(cell))
								{
									continue;
								}
								const std::uint16_t paletteIndex = tilecell::PaletteIndex(cell);
								if (paletteIndex >= map.tilePalette.size())
								{
									continue;
								}
								const TileDefinition* tile = tileSet.Find(map.tilePalette[paletteIndex]);
								if (tile == nullptr)
								{
									continue;
								}
								const auto atlasResult = m_sprites->LoadAtlas(tile->atlasPath);
								if (!atlasResult.has_value())
								{
									continue;
								}
								const SpriteAtlasAsset& atlas = **atlasResult;
								const SpriteRegion* region = atlas.Find(tile->spriteId);
								if (region == nullptr)
								{
									continue;
								}

								const glm::vec2 cellCentre{(static_cast<float>(chunkKey.x * kTileChunkSize + localX) + 0.5f) * cellSize, (static_cast<float>(chunkKey.y * kTileChunkSize + localY) + 0.5f) * cellSize};

								SpriteInstanceFlags flags = atlas.filterRecommendation == "nearest" ? SpriteInstanceFlags::NearestFilter : SpriteInstanceFlags::None;
								if ((cell & tilecell::kFlipX) != 0u)
								{
									flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(SpriteInstanceFlags::FlipX));
								}
								if ((cell & tilecell::kFlipY) != 0u)
								{
									flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(SpriteInstanceFlags::FlipY));
								}

								const TextureHandle texture = ResolveTexture(atlas.texturePath);
								std::uint32_t textureSlot = m_textures != nullptr ? m_textures->ResolveSlot(texture) : kInvalidTextureSlot;
								if (textureSlot == kInvalidTextureSlot && m_textures != nullptr)
								{
									textureSlot = m_textures->ResolveSlot(m_textures->DefaultHandle());
								}

								// Solid cells cast 2D shadows. Carry the tile's texture region so the occluder
								// mask samples the ARTWORK's alpha - shadows follow the drawn shape, not the cell.
								//
								// The LAYER has to be a collision layer too, exactly as IsWorldPointSolid requires.
								// Testing only the tile meant a cell painted on a non-colliding backdrop threw hard
								// shadows and swallowed light while the player walked straight through it - a shadow
								// with nothing casting it. Solidity and shadow-casting answer to the same rule.
								if (layer.collision && tile->collision != TileCollisionKind::None)
								{
									const glm::vec2 worldCentre = glm::vec2(entityTransform * glm::vec4(cellCentre, 0.0f, 1.0f));
									std::uint32_t occFlags = 0;
									if ((cell & tilecell::kFlipX) != 0u)
									{
										occFlags |= 1u;
									}
									if ((cell & tilecell::kFlipY) != 0u)
									{
										occFlags |= 2u;
									}
									cache.occluders.push_back(Occluder2D{
									        .posHalfSize = glm::vec4(worldCentre, cellSize * 0.5f, 0.0f),
									        .uvRect = InsetAtlasUvRect(region->uvRect, atlas.textureWidth, atlas.textureHeight),
									        .textureIndex = textureSlot,
									        .flags = occFlags,
									});
								}

								if (!tile->animationFrames.empty())
								{
									cache.animated.push_back({static_cast<std::uint32_t>(cache.instances.size()), static_cast<std::uint32_t>(tile - tileSet.tiles.data())});
								}

								cache.instances.push_back(SpriteRenderInstance{
								        .world = entityTransform * glm::translate(glm::mat4{1.0f}, glm::vec3{cellCentre, 0.0f}),
								        .uvRect = InsetAtlasUvRect(region->uvRect, atlas.textureWidth, atlas.textureHeight),
								        .color = color,
								        .sizeAndPivot = {cellSize, cellSize, 0.5f, 0.5f},
								        .sortKey = sortKey,
								        .textureIndex = textureSlot,
								        .entityId = entity.id,
								        .flags = flags,
								        .blendMode = 0, // alpha
								});
							}
						}
					}

					output.occluders.insert(output.occluders.end(), cache.occluders.begin(), cache.occluders.end());

					const std::size_t firstAppended = output.sprites.size();
					output.sprites.insert(output.sprites.end(), cache.instances.begin(), cache.instances.end());

					// Animated tiles: patch the appended copies' UVs to the frame for
					// the current time; the cache stays untouched.
					for (const AnimatedSlot& slot: cache.animated)
					{
						if (slot.tileIndex >= tileSet.tiles.size())
						{
							continue;
						}
						const TileDefinition& tile = tileSet.tiles[slot.tileIndex];
						if (tile.animationFrames.empty())
						{
							continue;
						}
						const std::size_t frameIndex = static_cast<std::size_t>(std::max(elapsedSeconds, 0.0f) * tile.animationFps) % tile.animationFrames.size();
						const auto atlasResult = m_sprites->LoadAtlas(tile.atlasPath);
						if (!atlasResult.has_value())
						{
							continue;
						}
						const SpriteRegion* frameRegion = (*atlasResult)->Find(tile.animationFrames[frameIndex]);
						if (frameRegion == nullptr)
						{
							continue;
						}
						output.sprites[firstAppended + slot.instanceIndex].uvRect = InsetAtlasUvRect(frameRegion->uvRect, (*atlasResult)->textureWidth, (*atlasResult)->textureHeight);
					}
				}
			}
		}
	}
} // namespace aether
