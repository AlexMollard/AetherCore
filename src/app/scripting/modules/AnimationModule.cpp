#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "animation/ModelAnimator.hpp"

namespace
{
	using namespace aether::app::scripting;

	// ── Playback control ──────────────────────────────────────────────────────

	void das_set_animation(aether::World* w, uint32_t id, int32_t clipIndex)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator || clipIndex < 0)
		{
			return;
		}
		ac->animator->SetAnimation(static_cast<std::uint32_t>(clipIndex));
	}

	int32_t das_get_current_animation(aether::World* w, uint32_t id)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return -1;
		}
		return static_cast<int32_t>(ac->animator->GetCurrentAnimation());
	}

	void das_set_playback_speed(aether::World* w, uint32_t id, float speed)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return;
		}
		ac->animator->SetPlaybackSpeed(speed);
	}

	float das_get_playback_speed(aether::World* w, uint32_t id)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return 0.0f;
		}
		return ac->animator->GetPlaybackSpeed();
	}

	void das_set_anim_time(aether::World* w, uint32_t id, float t)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return;
		}
		ac->animator->SetAnimTime(t);
	}

	float das_get_anim_time(aether::World* w, uint32_t id)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return 0.0f;
		}
		return ac->animator->GetAnimTime();
	}

	// ── Clip queries ──────────────────────────────────────────────────────────

	int32_t das_get_animation_count(aether::World* w, uint32_t id)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return 0;
		}
		return static_cast<int32_t>(ac->animator->GetAnimationCount());
	}

	const char* das_get_animation_name(aether::World* w, uint32_t id, int32_t index)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator || index < 0)
		{
			return nullptr;
		}
		auto uIndex = static_cast<std::uint32_t>(index);
		if (uIndex >= ac->animator->GetAnimationCount())
		{
			return nullptr;
		}
		auto sv = ac->animator->GetAnimationName(uIndex);
		return sv.data();
	}

	float das_get_animation_duration(aether::World* w, uint32_t id)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator)
		{
			return 0.0f;
		}
		return ac->animator->GetDuration();
	}

	int32_t das_find_animation(aether::World* w, uint32_t id, const char* name)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac || !ac->animator || !name)
		{
			return -1;
		}
		uint32_t count = ac->animator->GetAnimationCount();
		for (uint32_t i = 0; i < count; ++i)
		{
			if (ac->animator->GetAnimationName(i) == name)
			{
				return static_cast<int32_t>(i);
			}
		}
		return -1;
	}

	// ── AnimatorComponent field access ────────────────────────────────────────

	void das_set_animator_hero(aether::World* w, uint32_t id, bool hero)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac)
		{
			return;
		}
		ac->heroCharacter = hero;
	}

	void das_set_animator_lod(aether::World* w, uint32_t id, int32_t tier)
	{
		auto* ac = w->TryGet<aether::AnimatorComponent>(aether::Entity{ id });
		if (!ac)
		{
			return;
		}
		ac->lodTier = static_cast<std::uint8_t>(tier);
	}

	// ── Entity iteration ──────────────────────────────────────────────────────

	// for_each_with_animator(world) <| $(e : uint) { ... }
	void das_for_each_with_animator(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		for (auto enttE: w->View<aether::AnimatorComponent>())
		{
			const uint32_t id = static_cast<uint32_t>(entt::to_integral(enttE));
			vec4f args[1];
			args[0] = das::cast<uint32_t>::from(id);
			ctx->invoke(block, args, nullptr, at);
		}
	}

} // namespace

namespace aether::app::scripting
{
	struct AnimationModule : DasModuleBase
	{
		AnimationModule()
		      : DasModuleBase("animation")
		{
			das::ModuleLibrary lib(this);

			// Playback control
			Bind<das_set_animation>(lib, "set_animation", SE::modifyExternal);
			Bind<das_get_current_animation>(lib, "get_current_animation", SE::accessExternal);
			Bind<das_set_playback_speed>(lib, "set_playback_speed", SE::modifyExternal);
			Bind<das_get_playback_speed>(lib, "get_playback_speed", SE::accessExternal);
			Bind<das_set_anim_time>(lib, "set_anim_time", SE::modifyExternal);
			Bind<das_get_anim_time>(lib, "get_anim_time", SE::accessExternal);

			// Clip queries
			Bind<das_get_animation_count>(lib, "get_animation_count", SE::accessExternal);
			Bind<das_get_animation_name>(lib, "get_animation_name", SE::accessExternal);
			Bind<das_get_animation_duration>(lib, "get_animation_duration", SE::accessExternal);
			Bind<das_find_animation>(lib, "find_animation", SE::accessExternal);

			// AnimatorComponent field access
			Bind<das_set_animator_hero>(lib, "set_animator_hero", SE::modifyExternal);
			Bind<das_set_animator_lod>(lib, "set_animator_lod", SE::modifyExternal);

			// Entity iteration
			Bind<das_for_each_with_animator>(lib, "for_each_with_animator", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(AnimationModule, aether::app::scripting)
