#include "rendering/SpriteSystem.hpp"

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
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

		[[nodiscard]] std::uint64_t MakeSortKey(const SpriteRendererComponent& sprite, Entity entity) noexcept
		{
			const std::uint64_t layer = static_cast<std::uint64_t>(BiasSigned(sprite.sortingLayer));
			const std::uint64_t order = static_cast<std::uint64_t>(BiasSigned(sprite.orderInLayer));
			return (layer << 48u) | (order << 32u) | static_cast<std::uint64_t>(entity.id);
		}
	} // namespace

	void SpriteSystem::Initialize(TextureRegistry& textures, SpriteAssetStore& assets)
	{
		m_textures = &textures;
		m_assets = &assets;
	}

	void SpriteSystem::Shutdown()
	{
		if (m_textures != nullptr)
		{
			for (const auto& [path, handle]: m_textureCache)
			{
				(void) path;
				m_textures->Release(handle);
			}
		}
		m_textureCache.clear();
		m_textures = nullptr;
		m_assets = nullptr;
	}

	TextureHandle SpriteSystem::ResolveTexture(const std::string& path)
	{
		if (m_textures == nullptr || path.empty())
		{
			return m_textures != nullptr ? m_textures->DefaultHandle() : TextureHandle{};
		}
		if (const auto it = m_textureCache.find(path); it != m_textureCache.end())
		{
			return it->second;
		}
		const TextureHandle handle = m_textures->Acquire(path);
		m_textureCache.emplace(path, handle);
		return handle;
	}

	void SpriteSystem::Extract(World& world, Render2DFrameData& output)
	{
		AE_PROFILE_ZONE();
		output.sprites.clear();
		auto view = world.GetRegistry().view<const TransformComponent, const SpriteRendererComponent>(entt::exclude<DisabledComponent>);
		output.sprites.reserve(view.size_hint());
		for (const entt::entity raw: view)
		{
			const auto& transform = view.get<const TransformComponent>(raw);
			const auto& sprite = view.get<const SpriteRendererComponent>(raw);
			if (!sprite.visible)
			{
				continue;
			}

			const Entity entity = World::FromEntt(raw);
			std::string_view texturePath = sprite.texturePath;
			glm::vec4 uvRect = sprite.uvRect;
			glm::vec2 pixelSize = sprite.pixelSize;
			glm::vec2 pivot = sprite.pivot;
			float pixelsPerUnit = sprite.pixelsPerUnit;
			if (m_assets != nullptr && !sprite.atlasPath.empty() && sprite.spriteId.IsValid())
			{
				if (const auto atlasResult = m_assets->LoadAtlas(sprite.atlasPath); atlasResult.has_value())
				{
					const SpriteAtlasAsset& atlas = **atlasResult;
					if (const SpriteRegion* region = atlas.Find(sprite.spriteId))
					{
						texturePath = atlas.texturePath;
						uvRect = region->uvRect;
						pixelSize = region->pixelSize;
						pivot = region->pivot;
						pixelsPerUnit = atlas.pixelsPerUnit;
					}
				}
			}
			const TextureHandle texture = ResolveTexture(std::string(texturePath));
			std::uint32_t textureSlot = m_textures != nullptr ? m_textures->ResolveSlot(texture) : kInvalidTextureSlot;
			if (textureSlot == kInvalidTextureSlot && m_textures != nullptr)
			{
				textureSlot = m_textures->ResolveSlot(m_textures->DefaultHandle());
			}

			SpriteInstanceFlags flags = SpriteInstanceFlags::None;
			if (sprite.flipX)
			{
				flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(SpriteInstanceFlags::FlipX));
			}
			if (sprite.flipY)
			{
				flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(SpriteInstanceFlags::FlipY));
			}
			if (sprite.pixelSnap)
			{
				flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(SpriteInstanceFlags::PixelSnap));
			}

			const float ppu = std::max(pixelsPerUnit, 0.001f);
			output.sprites.push_back(SpriteRenderInstance{
			        .world = transform.localToWorld,
			        .uvRect = uvRect,
			        .color = sprite.tint,
			        .sizeAndPivot = {pixelSize.x / ppu, pixelSize.y / ppu, pivot.x, pivot.y},
			        .sortKey = MakeSortKey(sprite, entity),
			        .textureIndex = textureSlot,
			        .entityId = entity.id,
			        .flags = flags,
			        .blendMode = static_cast<std::uint32_t>(sprite.blendMode),
			});
		}

		std::ranges::stable_sort(output.sprites, [](const SpriteRenderInstance& a, const SpriteRenderInstance& b) { return a.sortKey < b.sortKey; });
	}
} // namespace aether
