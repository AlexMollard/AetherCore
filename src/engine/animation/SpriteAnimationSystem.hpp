#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether
{
	struct SpriteAnimationAsset;
	class SpriteAssetStore;
	struct SpriteAnimatorComponent;
	struct SpriteRendererComponent;

	struct SpriteAnimationEventRecord
	{
		Entity entity{};
		std::string name;
		std::string payload;
		std::uint32_t frameIndex = 0;
	};

	class SpriteAnimationSystem final : public System
	{
	public:
		static constexpr float kFixedTimestep = 1.0f / 60.0f;
		static constexpr std::size_t kMaxQueuedEvents = 256;

		explicit SpriteAnimationSystem(SpriteAssetStore& assets);

		[[nodiscard]] const char* GetName() const override
		{
			return "SpriteAnimationSystem";
		}

		void Update(World& world, float dt) override;
		[[nodiscard]] bool TryPopEvent(SpriteAnimationEventRecord& event);
		[[nodiscard]] bool TryPopEvent(Entity entity, SpriteAnimationEventRecord& event);
		[[nodiscard]] std::size_t PendingEventCount() const noexcept
		{
			return m_events.size();
		}

	private:
		void ApplyFrame(SpriteRendererComponent& renderer, const SpriteAnimationAsset& animation, const SpriteAnimatorComponent& animator);
		void EnterFrame(Entity entity, SpriteAnimatorComponent& animator, const SpriteAnimationAsset& animation, std::uint32_t frameIndex);
		void Advance(Entity entity, SpriteAnimatorComponent& animator, const SpriteAnimationAsset& animation, float stepSeconds);

		SpriteAssetStore& m_assets;
		std::deque<SpriteAnimationEventRecord> m_events;
	};
} // namespace aether
