#include "animation/SpriteAnimationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	SpriteAnimationSystem::SpriteAnimationSystem(SpriteAssetStore& assets)
	      : m_assets(assets)
	{
	}

	void SpriteAnimationSystem::Update(World& world, float dt)
	{
		UpdateAnimations(world, dt, true);
	}

	void SpriteAnimationSystem::UpdatePreview(World& world, float dt)
	{
		if (!m_previewEnabled)
		{
			return;
		}
		UpdateAnimations(world, dt, false);
	}

	void SpriteAnimationSystem::ResetForPlay(World& world)
	{
		m_events.clear();
		auto view = world.GetRegistry().view<SpriteAnimatorComponent>();
		for (const entt::entity raw: view)
		{
			auto& animator = view.get<SpriteAnimatorComponent>(raw);
			animator.frameTime = 0.0f;
			animator.fixedAccumulator = 0.0f;
			animator.currentFrame = animator.startFrame;
			animator.direction = 1;
			animator.playing = false;
			animator.initialized = false;
		}
	}

	void SpriteAnimationSystem::UpdateAnimations(World& world, float dt, bool emitEvents)
	{
		AE_PROFILE_ZONE();
		auto view = world.GetRegistry().view<SpriteAnimatorComponent, SpriteRendererComponent>(entt::exclude<DisabledComponent>);
		for (const entt::entity raw: view)
		{
			const Entity entity = World::FromEntt(raw);
			if (ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			auto& animator = view.get<SpriteAnimatorComponent>(raw);
			auto& renderer = view.get<SpriteRendererComponent>(raw);
			const auto animationResult = m_assets.LoadAnimation(animator.animationPath);
			if (!animationResult.has_value() || (*animationResult)->frames.empty())
			{
				continue;
			}
			const SpriteAnimationAsset& animation = **animationResult;
			if (!animator.initialized)
			{
				animator.currentFrame = std::min<std::uint32_t>(animator.startFrame, static_cast<std::uint32_t>(animation.frames.size() - 1));
				animator.frameTime = 0.0f;
				animator.fixedAccumulator = 0.0f;
				animator.direction = 1;
				animator.playing = animator.autoplay;
				animator.initialized = true;
				ApplyFrame(renderer, animation, animator);
			}
			if (!animator.playing)
			{
				ApplyFrame(renderer, animation, animator);
				continue;
			}

			animator.fixedAccumulator = std::min(animator.fixedAccumulator + std::max(dt, 0.0f), 0.25f);
			while (animator.fixedAccumulator >= kFixedTimestep)
			{
				Advance(entity, animator, animation, kFixedTimestep * std::max(animator.speed, 0.0f), emitEvents);
				animator.fixedAccumulator -= kFixedTimestep;
			}
			ApplyFrame(renderer, animation, animator);
		}
	}

	bool SpriteAnimationSystem::TryPopEvent(SpriteAnimationEventRecord& event)
	{
		if (m_events.empty())
		{
			return false;
		}
		event = std::move(m_events.front());
		m_events.pop_front();
		return true;
	}

	bool SpriteAnimationSystem::TryPopEvent(Entity entity, SpriteAnimationEventRecord& event)
	{
		const auto it = std::ranges::find(m_events, entity, &SpriteAnimationEventRecord::entity);
		if (it == m_events.end())
		{
			return false;
		}
		event = std::move(*it);
		m_events.erase(it);
		return true;
	}

	void SpriteAnimationSystem::ApplyFrame(SpriteRendererComponent& renderer, const SpriteAnimationAsset& animation, const SpriteAnimatorComponent& animator)
	{
		if (animation.frames.empty())
		{
			return;
		}
		const std::uint32_t frameIndex = std::min<std::uint32_t>(animator.currentFrame, static_cast<std::uint32_t>(animation.frames.size() - 1));
		const AssetObjectId spriteId = animation.frames[frameIndex].spriteId;
		const auto atlasResult = m_assets.LoadAtlas(animation.atlasPath);
		if (!atlasResult.has_value())
		{
			return;
		}
		const SpriteAtlasAsset& atlas = **atlasResult;
		const SpriteRegion* region = atlas.Find(spriteId);
		if (region == nullptr)
		{
			return;
		}
		renderer.atlasPath = animation.atlasPath;
		renderer.texturePath = atlas.texturePath;
		renderer.spriteId = region->id;
		renderer.uvRect = region->uvRect;
		renderer.pixelSize = region->pixelSize;
		renderer.pivot = region->pivot;
		renderer.pixelsPerUnit = atlas.pixelsPerUnit;
	}

	void SpriteAnimationSystem::EnterFrame(Entity entity, SpriteAnimatorComponent& animator, const SpriteAnimationAsset& animation, std::uint32_t frameIndex, bool emitEvents)
	{
		animator.currentFrame = frameIndex;
		if (!emitEvents)
		{
			return;
		}
		for (const SpriteAnimationEvent& event: animation.events)
		{
			if (event.frameIndex != frameIndex || event.name.empty())
			{
				continue;
			}
			if (m_events.size() == kMaxQueuedEvents)
			{
				m_events.pop_front();
			}
			m_events.push_back({entity, event.name, event.payload, frameIndex});
		}
	}

	void SpriteAnimationSystem::Advance(Entity entity, SpriteAnimatorComponent& animator, const SpriteAnimationAsset& animation, float stepSeconds, bool emitEvents)
	{
		if (animation.frames.empty() || stepSeconds <= 0.0f)
		{
			return;
		}
		const SpriteAnimationLoopMode mode = animator.useAssetLoopMode ? animation.loopMode : animator.loopMode;
		animator.frameTime += stepSeconds;
		std::uint32_t transitions = 0;
		while (transitions++ < 128)
		{
			const std::uint32_t current = std::min<std::uint32_t>(animator.currentFrame, static_cast<std::uint32_t>(animation.frames.size() - 1));
			const float duration = std::max(animation.frames[current].durationSeconds, 0.001f);
			if (animator.frameTime < duration)
			{
				break;
			}
			animator.frameTime -= duration;
			if (mode == SpriteAnimationLoopMode::Loop)
			{
				EnterFrame(entity, animator, animation, (current + 1u) % static_cast<std::uint32_t>(animation.frames.size()), emitEvents);
			}
			else if (mode == SpriteAnimationLoopMode::PingPong)
			{
				if (animation.frames.size() == 1)
				{
					animator.frameTime = std::fmod(animator.frameTime, duration);
					break;
				}
				std::int32_t next = static_cast<std::int32_t>(current) + animator.direction;
				if (next >= static_cast<std::int32_t>(animation.frames.size()))
				{
					animator.direction = -1;
					next = static_cast<std::int32_t>(animation.frames.size()) - 2;
				}
				else if (next < 0)
				{
					animator.direction = 1;
					next = 1;
				}
				EnterFrame(entity, animator, animation, static_cast<std::uint32_t>(next), emitEvents);
			}
			else if (current + 1u < animation.frames.size())
			{
				EnterFrame(entity, animator, animation, current + 1u, emitEvents);
			}
			else
			{
				animator.frameTime = mode == SpriteAnimationLoopMode::Hold ? duration : 0.0f;
				animator.playing = false;
				break;
			}
		}
	}
} // namespace aether
