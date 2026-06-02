#include "scripting/DasModuleBase.hpp"

#include <algorithm>

#include "daScript/daScript.h"

#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace
{
	using namespace aether::app::scripting;

	// ── Helpers ───────────────────────────────────────────────────────────────

	// Find a SkinnedMeshComponent on entity or any spawned child.
	const aether::SkinnedMeshComponent* FindSmcOrSpawned(const aether::World* w, uint32_t id)
	{
		const aether::Entity e{id};
		if (const auto* smc = w->TryGet<aether::SkinnedMeshComponent>(e))
		{
			return smc;
		}
		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(e);
		if (sec && !sec->entityIds.empty())
		{
			return w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{sec->entityIds.front()});
		}
		return nullptr;
	}

	aether::SkinnedMeshComponent* FindSmcOrSpawned(aether::World* w, uint32_t id)
	{
		const aether::Entity e{id};
		if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(e))
		{
			return smc;
		}
		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(e);
		if (sec && !sec->entityIds.empty())
		{
			return w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{sec->entityIds.front()});
		}
		return nullptr;
	}

	void ForEachSpawnedSmc(aether::World* w, uint32_t id, auto&& f)
	{
		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (!sec)
		{
			return;
		}
		for (const auto eid: sec->entityIds)
		{
			if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{eid}))
			{
				f(*smc);
			}
		}
	}

	// ── Playback control ──────────────────────────────────────────────────────

	void das_set_animation(aether::World* w, uint32_t id, int32_t clipIndex)
	{
		if (clipIndex < 0)
		{
			return;
		}

		auto clampAndSet = [clipIndex](aether::SkinnedMeshComponent& smc)
		{
			if (!smc.animDb)
			{
				return;
			}
			const auto maxClip = static_cast<int32_t>(smc.animDb->GetClipCount());
			if (maxClip <= 0)
			{
				return;
			}
			smc.clipIndex = static_cast<std::uint32_t>(std::min(clipIndex, maxClip - 1));
			smc.animTime = 0.f;
		};

		if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			clampAndSet(*smc);
		}

		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (sec)
		{
			for (const auto eid: sec->entityIds)
			{
				if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{eid}))
				{
					clampAndSet(*smc);
				}
			}
		}
	}

	int32_t das_get_current_animation(aether::World* w, uint32_t id)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		return smc ? static_cast<int32_t>(smc->clipIndex) : -1;
	}

	void das_set_playback_speed(aether::World* w, uint32_t id, float speed)
	{
		if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			smc->playbackSpeed = speed;
		}
		ForEachSpawnedSmc(w, id, [speed](aether::SkinnedMeshComponent& smc) { smc.playbackSpeed = speed; });
	}

	float das_get_playback_speed(aether::World* w, uint32_t id)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		return smc ? smc->playbackSpeed : 0.f;
	}

	void das_set_anim_time(aether::World* w, uint32_t id, float t)
	{
		if (auto* smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			smc->animTime = t;
		}
		ForEachSpawnedSmc(w, id, [t](aether::SkinnedMeshComponent& smc) { smc.animTime = t; });
	}

	float das_get_anim_time(aether::World* w, uint32_t id)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		return smc ? smc->animTime : 0.f;
	}

	// ── Clip queries ──────────────────────────────────────────────────────────

	int32_t das_get_animation_count(aether::World* w, uint32_t id)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		return (smc && smc->animDb) ? static_cast<int32_t>(smc->animDb->GetClipCount()) : 0;
	}

	const char* das_get_animation_name(aether::World* w, uint32_t id, int32_t index)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		if (!smc || !smc->animDb || index < 0)
		{
			return nullptr;
		}
		const auto uIdx = static_cast<std::uint32_t>(index);
		if (uIdx >= smc->animDb->GetClipCount())
		{
			return nullptr;
		}
		return smc->animDb->GetClipName(uIdx).data();
	}

	float das_get_animation_duration(aether::World* w, uint32_t id)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		if (!smc || !smc->animDb)
		{
			return 0.f;
		}
		return smc->animDb->GetClipDuration(smc->clipIndex);
	}

	int32_t das_find_animation(aether::World* w, uint32_t id, const char* name)
	{
		const auto* smc = FindSmcOrSpawned(w, id);
		if (!smc || !smc->animDb || !name)
		{
			return -1;
		}
		const std::uint32_t count = smc->animDb->GetClipCount();
		for (std::uint32_t i = 0; i < count; ++i)
		{
			if (smc->animDb->GetClipName(i) == name)
			{
				return static_cast<int32_t>(i);
			}
		}
		return -1;
	}

	// ── Entity iteration ──────────────────────────────────────────────────────

	void das_for_each_with_animator(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		for (auto enttE: w->View<aether::SkinnedMeshComponent>())
		{
			const uint32_t eid = static_cast<uint32_t>(entt::to_integral(enttE));
			vec4f args[1];
			args[0] = das::cast<uint32_t>::from(eid);
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

			// Entity iteration (name kept for script backward compatibility)
			Bind<das_for_each_with_animator>(lib, "for_each_with_animator", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(AnimationModule, aether::app::scripting)
