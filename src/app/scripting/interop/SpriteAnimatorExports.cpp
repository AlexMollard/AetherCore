#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>

#include "animation/SpriteAnimationSystem.hpp"
#include "scripting/SceneContext.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting::interop;

namespace
{
	aether::SpriteAnimatorComponent* Animator(std::uint32_t id)
	{
		return ActiveWorld().TryGet<aether::SpriteAnimatorComponent>(aether::Entity{id});
	}

	void CopyString(const std::string& source, char* destination, std::int32_t capacity)
	{
		if (destination == nullptr || capacity <= 0)
		{
			return;
		}
		const std::size_t count = std::min(source.size(), static_cast<std::size_t>(capacity - 1));
		std::memcpy(destination, source.data(), count);
		destination[count] = '\0';
	}
} // namespace

AE_SCRIPT_API void aether_sprite_animator_set_animation(std::uint32_t id, const char* path)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->animationPath = path != nullptr ? path : "";
		animator->initialized = false;
	}
	});
}

AE_SCRIPT_API void aether_sprite_animator_play(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->playing = true;
		animator->initialized = true;
	}
	});
}

AE_SCRIPT_API void aether_sprite_animator_pause(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->playing = false;
	}
	});
}

AE_SCRIPT_API void aether_sprite_animator_restart(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->currentFrame = animator->startFrame;
		animator->frameTime = 0.0f;
		animator->fixedAccumulator = 0.0f;
		animator->direction = 1;
		animator->playing = true;
		animator->initialized = true;
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_animator_is_playing(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* animator = Animator(id);
	return animator != nullptr && animator->playing ? 1 : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_sprite_animator_get_current_frame(std::uint32_t id)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* animator = Animator(id);
	return animator != nullptr ? animator->currentFrame : 0;
	});
}

AE_SCRIPT_API float aether_sprite_animator_get_speed(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto* animator = Animator(id);
	return animator != nullptr ? animator->speed : 0.0f;
	});
}

AE_SCRIPT_API void aether_sprite_animator_set_speed(std::uint32_t id, float value)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->speed = std::max(value, 0.0f);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_animator_get_loop_mode(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* animator = Animator(id);
	return animator != nullptr ? static_cast<std::int32_t>(animator->loopMode) : 0;
	});
}

AE_SCRIPT_API void aether_sprite_animator_set_loop_mode(std::uint32_t id, std::int32_t value)
{
	SafeExport([&] -> void
	{
	if (auto* animator = Animator(id))
	{
		animator->loopMode = static_cast<aether::SpriteAnimationLoopMode>(std::clamp(value, 0, 3));
		animator->useAssetLoopMode = false;
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_animator_pop_event(std::uint32_t id, char* name, std::int32_t nameCapacity, char* payload, std::int32_t payloadCapacity, std::uint32_t* frameIndex)
{
	return SafeExport([&] -> std::int32_t
	{
	auto* services = aether::app::scripting::ActiveContext().services;
	auto* system = services != nullptr ? services->TryGet<aether::SpriteAnimationSystem>() : nullptr;
	if (system == nullptr)
	{
		return 0;
	}
	aether::SpriteAnimationEventRecord event;
	if (!system->TryPopEvent(aether::Entity{id}, event))
	{
		return 0;
	}
	CopyString(event.name, name, nameCapacity);
	CopyString(event.payload, payload, payloadCapacity);
	if (frameIndex != nullptr)
	{
		*frameIndex = event.frameIndex;
	}
	return 1;
	});
}
