#include "rendering/SpriteSystem.hpp"

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

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

	void SpriteSystem::Initialize(TextureRegistry& textures)
	{
		m_textures = &textures;
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
			const TextureHandle texture = ResolveTexture(sprite.texturePath);
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

			const float ppu = std::max(sprite.pixelsPerUnit, 0.001f);
			output.sprites.push_back(SpriteRenderInstance{
			        .world = transform.localToWorld,
			        .uvRect = sprite.uvRect,
			        .color = sprite.tint,
			        .sizeAndPivot = {sprite.pixelSize.x / ppu, sprite.pixelSize.y / ppu, sprite.pivot.x, sprite.pivot.y},
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
