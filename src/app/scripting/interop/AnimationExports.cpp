#include <mutex>
#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scripting/Gameplay3D.hpp"
#include "assets/ExternalAnimation.hpp"
#include "io/FileSystem.hpp"
#if !defined(AETHERCORE_WEB_RUNTIME)
#	include "BinaryFormats.hpp"
#	include "IEngineRuntime.hpp"
#	include "animation/AnimationCompiler.hpp"
#	include "animation/AnimationDatabase.hpp"
#	include "rendering/RagdollSkinDrive.hpp"
#	include "utils/BinaryReader.hpp"
#endif
#include "animation/AnimationSystem.hpp"
#include "assets/GltfAsset.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	using aether::scripting::gameplay3d::FindSmcOrSpawned;
	using aether::scripting::gameplay3d::ForEachSpawnedSmc;

	std::int32_t AddAnimationImpl(aether::World& world, std::uint32_t id, const char* path, bool lockRoot)
	{
		const auto animator = aether::scripting::gameplay3d::FindAnimator(world, id);
		if (!animator.smc) return -1;
		auto data = aether::io::FileSystem::ReadFile(std::string(path ? path : ""));
		if (!data) return -1;
		auto animation = aether::assets::ParseExternalAnimation(*data);
		if (!animation)
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: invalid animation file: {}", path ? path : "");
			return -1;
		}
#if !defined(AETHERCORE_WEB_RUNTIME)
		if (const auto* db = animator.smc->animDb; db && db->GetSkinCount())
		{
			const auto& joints = db->GetSkinJoints();
			aether::assets::RemapExternalAnimation(*animation, std::span(joints.data(), db->GetSkinJointCount(0)), [db](auto node) { return db->GetNodeName(node); });
		}
#else
		if (const auto* catalog = ActiveContext().clips) catalog->Remap(animator.entity, *animation);
#endif
		animation->rootLocked = lockRoot;
		const auto index = aether::scripting::gameplay3d::ClipCount(animator, ActiveContext().clips);
		animator.smc->pendingExternalAnims.push_back(std::move(*animation));
		return static_cast<std::int32_t>(index);
	}
} // namespace

AE_SCRIPT_API std::int32_t aether_anim_add(std::uint32_t id, const char* animPath, std::int32_t lockRoot)
{ return SafeExport([&] -> std::int32_t { return AddAnimationImpl(ActiveWorld(), id, animPath, lockRoot != 0); }); }

AE_SCRIPT_API std::int32_t aether_anim_load_external(std::uint32_t id, const char* animPath)
{ return SafeExport([&] -> std::int32_t { return AddAnimationImpl(ActiveWorld(), id, animPath, false); }); }

AE_SCRIPT_API void aether_anim_compile(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	auto& world = ActiveWorld();
	if (FindSmcOrSpawned(world, id) == nullptr)
	{
		AE_WARN(aether::LogCategory::Animation, "compile_animations: entity {} has no SkinnedMeshComponent", id);
		return;
	}

#if defined(AETHERCORE_WEB_RUNTIME)
	if (ctx.clips) ctx.clips->Compile(world, aether::Entity{id});
#else
	// AppendAnimations reallocates GPU animation buffers the render thread reads.
	if (ctx.engineRuntime != nullptr)
	{
		ctx.engineRuntime->RunExclusive(aether::QuiesceMode::Drain, [&world, id]() { aether::CompileAnimations(world, id); });
	}
	else
	{
		aether::CompileAnimations(world, id);
	}
#endif
	});
}

AE_SCRIPT_API void aether_anim_clear_pending(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	auto& w = ActiveWorld();
	const auto clearFn = [](aether::SkinnedMeshComponent& smc)
	{
		smc.pendingExternalAnims.clear();
	};
	if (auto* smc = w.TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
	{
		clearFn(*smc);
	}
	ForEachSpawnedSmc(w, id, clearFn);
	});
}

AE_SCRIPT_API void aether_anim_set_clip(std::uint32_t id, std::int32_t clipIndex)
{
	SafeExport([&] -> void
	{
	if (clipIndex < 0)
	{
		return;
	}
	aether::scripting::gameplay3d::CrossFade(ActiveWorld(), id, static_cast<std::uint32_t>(clipIndex), 0.0f);
	});
}

AE_SCRIPT_API std::int32_t aether_anim_get_current(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* smc = FindSmcOrSpawned(ActiveWorld(), id);
	return smc != nullptr ? static_cast<std::int32_t>(smc->clipIndex) : -1;
	});
}

AE_SCRIPT_API void aether_anim_set_playback_speed(std::uint32_t id, float speed)
{
	SafeExport([&] -> void
	{
	aether::scripting::gameplay3d::ForEachAnimator(ActiveWorld(), id, [speed](aether::SkinnedMeshComponent& smc) { smc.playbackSpeed = speed; });
	});
}

AE_SCRIPT_API float aether_anim_get_playback_speed(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto* smc = FindSmcOrSpawned(ActiveWorld(), id);
	return smc != nullptr ? smc->playbackSpeed : 0.0f;
	});
}

AE_SCRIPT_API void aether_anim_set_time(std::uint32_t id, float t)
{
	SafeExport([&] -> void
	{
	auto& w = ActiveWorld();
	if (auto* smc = w.TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
	{
		smc->animTime = t;
	}
	ForEachSpawnedSmc(w, id, [t](aether::SkinnedMeshComponent& smc) { smc.animTime = t; });
	});
}

AE_SCRIPT_API float aether_anim_get_time(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto* smc = FindSmcOrSpawned(ActiveWorld(), id);
	return smc != nullptr ? smc->animTime : 0.0f;
	});
}

AE_SCRIPT_API std::int32_t aether_anim_get_count(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto animator = aether::scripting::gameplay3d::FindAnimator(ActiveWorld(), id);
	return animator.smc != nullptr ? static_cast<std::int32_t>(aether::scripting::gameplay3d::ClipCount(animator, ActiveContext().clips)) : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_anim_get_name(std::uint32_t id, std::int32_t index, char* buf, std::int32_t bufLen)
{
	return SafeExport([&] -> std::int32_t
	{
	if (buf == nullptr || bufLen <= 0 || index < 0)
	{
		return 0;
	}
	const auto animator = aether::scripting::gameplay3d::FindAnimator(ActiveWorld(), id);
	if (animator.smc == nullptr)
	{
		return 0;
	}
	const std::string_view name = aether::scripting::gameplay3d::ClipName(animator, static_cast<std::uint32_t>(index), ActiveContext().clips);
	if (name.empty())
	{
		return 0;
	}

	const auto n = static_cast<std::int32_t>(name.size());
	const std::int32_t copy = n < bufLen - 1 ? n : bufLen - 1;
	std::memcpy(buf, name.data(), static_cast<size_t>(copy));
	buf[copy] = '\0';
	return copy;
	});
}

AE_SCRIPT_API float aether_anim_get_duration(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto animator = aether::scripting::gameplay3d::FindAnimator(ActiveWorld(), id);
	return animator.smc != nullptr ? aether::scripting::gameplay3d::ClipDuration(animator, animator.smc->clipIndex, ActiveContext().clips) : 0.0f;
	});
}

AE_SCRIPT_API std::int32_t aether_anim_find(std::uint32_t id, const char* name)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto animator = aether::scripting::gameplay3d::FindAnimator(ActiveWorld(), id);
	if (animator.smc == nullptr || name == nullptr)
	{
		return -1;
	}
	return aether::scripting::gameplay3d::FindClip(animator, name, ActiveContext().clips);
	});
}

AE_SCRIPT_API std::int32_t aether_anim_get_entities_with_animator(std::uint32_t* buf, std::int32_t cap)
{
	return SafeExport([&] -> std::int32_t
	{
	if (buf == nullptr || cap <= 0)
	{
		return 0;
	}
	std::int32_t n = 0;
	for (const auto enttE: ActiveWorld().View<aether::SkinnedMeshComponent>())
	{
		if (n >= cap)
		{
			break;
		}
		buf[n++] = aether::World::FromEntt(enttE).id;
	}
	return n;
	});
}

AE_SCRIPT_API void aether_anim_crossfade(std::uint32_t id, std::int32_t clipIndex, float seconds)
{
	SafeExport([&] -> void
	{
	if (clipIndex < 0)
	{
		return;
	}
	aether::scripting::gameplay3d::CrossFade(ActiveWorld(), id, static_cast<std::uint32_t>(clipIndex), seconds);
	});
}

AE_SCRIPT_API void aether_anim_set_layer_clip(std::uint32_t id, std::int32_t clipIndex)
{
	SafeExport([&] -> void
	{
	auto& w = ActiveWorld();
	const auto layer = [clipIndex](aether::SkinnedMeshComponent& smc)
	{
		if (clipIndex < 0)
		{
			smc.fadeWeight = 0.f;
			return;
		}
		// A partial clip (e.g. Crash's root-only spin) layered at full weight over the playing
		// clip: the sample shader overwrites exactly the nodes the layer animates and keeps the
		// rest of the playing clip's pose. fadeRate 0 pins the layer until it is cleared.
		smc.fadeClipIndex = static_cast<std::uint32_t>(clipIndex);
		smc.fadeTime = 0.f;
		smc.fadeLooping = true;
		smc.fadeWeight = 1.f;
		smc.fadeRate = 0.f;
	};
	if (auto* smc = w.TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
	{
		layer(*smc);
	}
	ForEachSpawnedSmc(w, id, layer);
	});
}

AE_SCRIPT_API void aether_anim_set_joint_offset(std::uint32_t id, const char* jointName, Vec4 rotation)
{
	SafeExport([&] -> void
	{
	if (jointName == nullptr)
	{
		return;
	}
#if defined(AETHERCORE_WEB_RUNTIME)
	auto& w = ActiveWorld();
	const glm::quat q = glm::normalize(glm::quat(rotation.w, rotation.x, rotation.y, rotation.z));
	const bool identity = std::abs(q.w) > 0.999999f;
	const auto apply = [&](aether::Entity entity)
	{
		auto* smc = w.TryGet<aether::SkinnedMeshComponent>(entity);
		const auto* catalog = ActiveContext().clips;
		if (!smc || !catalog) { return; }
		const auto node = catalog->FindNode(entity, jointName);
		if (!node) { return; }
		auto& offsets = smc->jointOffsets;
		const auto it = std::ranges::find(offsets, *node, &aether::JointRotationOffset::node);
		if (identity) { if (it != offsets.end()) { offsets.erase(it); } }
		else if (it != offsets.end()) { it->rotation = q; }
		else { offsets.push_back({.node = *node, .rotation = q}); }
	};
	apply(aether::Entity{id});
	if (const auto* hierarchy = w.TryGet<aether::HierarchyComponent>(aether::Entity{id}))
	{
		for (const auto child: hierarchy->children) { apply(child); }
	}
#else
	auto& w = ActiveWorld();
	// Interop Vec4 is xyzw; glm::quat's constructor takes w first.
	const glm::quat q = glm::normalize(glm::quat(rotation.w, rotation.x, rotation.y, rotation.z));
	const bool identity = std::abs(q.w) > 0.999999f;
	const auto apply = [&](aether::SkinnedMeshComponent& smc)
	{
		if (smc.animDb == nullptr || !smc.animDb->IsAlive() || !smc.animDb->IsValid())
		{
			return;
		}
		const std::optional<std::uint32_t> node = aether::FindNodeIndexByName(smc.animDb->GetNodeNames(), jointName);
		if (!node.has_value())
		{
			return;
		}
		auto& offsets = smc.jointOffsets;
		const auto it = std::ranges::find(offsets, *node, &aether::JointRotationOffset::node);
		if (identity)
		{
			if (it != offsets.end())
			{
				offsets.erase(it);
			}
		}
		else if (it != offsets.end())
		{
			it->rotation = q;
		}
		else
		{
			offsets.push_back({.node = *node, .rotation = q});
		}
	};
	if (auto* smc = w.TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
	{
		apply(*smc);
	}
	ForEachSpawnedSmc(w, id, apply);
#endif
	});
}

AE_SCRIPT_API void aether_anim_set_root_motion_enabled(std::uint32_t id, std::int32_t enabled)
{
	SafeExport([&] -> void
	{
	auto& w = ActiveWorld();
	const aether::Entity entity{id};
	const bool on = enabled != 0;
	if (auto* rmComp = w.TryGet<aether::RootMotionComponent>(entity))
	{
		rmComp->enabled = on;
	}
	ForEachSpawnedSmc(w,
	        id,
	        [&](aether::SkinnedMeshComponent&)
	        {
		        if (auto* comp = w.TryGet<aether::RootMotionComponent>(aether::Entity{id}))
		        {
			        comp->enabled = on;
		        }
	        });
	});
}

AE_SCRIPT_API std::int32_t aether_anim_get_root_motion_enabled(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* rmComp = ActiveWorld().TryGet<aether::RootMotionComponent>(aether::Entity{id});
	return rmComp != nullptr && rmComp->enabled ? 1 : 0;
	});
}

AE_SCRIPT_API Vec3 aether_anim_get_root_motion_delta(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	// Root motion is not implemented: poses are sampled on the GPU and the hips are never
	// read back, so nothing writes accumulatedDelta and this is always zero. Say so once -
	// a script driving a character from this sees no movement and no reason for it.
	static std::once_flag warnOnce;
	std::call_once(warnOnce,
	        [] { AE_WARN(aether::LogCategory::Animation, "Animation.GetRootMotionDelta: root motion is not implemented; this always returns zero."); });
	const auto* rmComp = ActiveWorld().TryGet<aether::RootMotionComponent>(aether::Entity{id});
	return rmComp != nullptr ? FromGlm(rmComp->accumulatedDelta) : Vec3{};
	});
}
