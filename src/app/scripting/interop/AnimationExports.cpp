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
#if !defined(AETHERCORE_WEB_RUNTIME)
#	include "BinaryFormats.hpp"
#	include "IEngineRuntime.hpp"
#	include "animation/AnimationCompiler.hpp"
#	include "animation/AnimationDatabase.hpp"
#	include "io/FileSystem.hpp"
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

#if !defined(AETHERCORE_WEB_RUNTIME)
	std::string StripBonePrefix(std::string name)
	{
		static constexpr const char* kPrefixes[] = {"mixamorig:", "mixamorig_", "Armature_"};
		for (const auto* prefix: kPrefixes)
		{
			const std::size_t plen = std::strlen(prefix);
			if (name.size() > plen && name.substr(0, plen) == prefix)
			{
				return name.substr(plen);
			}
		}
		return name;
	}

	std::unordered_map<std::string, std::uint32_t> BuildBoneNameMap(const aether::AnimationDatabase* animDb)
	{
		std::unordered_map<std::string, std::uint32_t> boneMap;
		if (animDb == nullptr || animDb->GetSkinCount() == 0)
		{
			return boneMap;
		}

		const auto jointCount = animDb->GetSkinJointCount(0);
		const auto& skinJoints = animDb->GetSkinJoints();
		static constexpr const char* kAltPrefixes[] = {"mixamorig:", "mixamorig_", "Armature_"};

		for (std::uint32_t i = 0; i < jointCount; ++i)
		{
			const auto nodeIdx = skinJoints[i];
			const std::string nodeName(animDb->GetNodeName(nodeIdx));
			if (nodeName.empty())
			{
				continue;
			}
			boneMap[nodeName] = nodeIdx;
			const std::string stripped = StripBonePrefix(nodeName);
			if (stripped != nodeName)
			{
				boneMap.try_emplace(stripped, nodeIdx);
			}
			if (!stripped.empty())
			{
				for (const auto* altPrefix: kAltPrefixes)
				{
					std::string alt = altPrefix + stripped;
					if (alt != nodeName)
					{
						boneMap.try_emplace(std::move(alt), nodeIdx);
					}
				}
			}
		}
		return boneMap;
	}

	void RemapAnimationByBoneName(aether::assets::GltfAnimation& anim, const std::unordered_map<std::string, std::uint32_t>& boneNameToJointIndex, const aether::AnimationDatabase* animDb)
	{
		bool hasBoneNames = false;
		for (auto& ch: anim.channels)
		{
			if (ch.boneName.empty())
			{
				continue;
			}
			hasBoneNames = true;
			auto it = boneNameToJointIndex.find(ch.boneName);
			if (it != boneNameToJointIndex.end())
			{
				ch.nodeIndex = it->second;
				continue;
			}
			const std::string animBare = StripBonePrefix(ch.boneName);
			if (animBare != ch.boneName)
			{
				it = boneNameToJointIndex.find(animBare);
				if (it != boneNameToJointIndex.end())
				{
					ch.nodeIndex = it->second;
				}
			}
		}

		if (!hasBoneNames && animDb != nullptr)
		{
			const auto jointCount = animDb->GetSkinJointCount(0);
			const auto& skinJoints = animDb->GetSkinJoints();
			for (auto& ch: anim.channels)
			{
				if (ch.nodeIndex < jointCount)
				{
					ch.nodeIndex = skinJoints[ch.nodeIndex];
				}
			}
		}
	}

	std::int32_t AddAnimationImpl(aether::World& w, std::uint32_t id, const char* animPath, bool lockRoot)
	{
		auto* smc = FindSmcOrSpawned(w, id);
		if (smc == nullptr)
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: entity {} has no SkinnedMeshComponent", id);
			return -1;
		}

		auto data = aether::io::FileSystem::ReadFile(std::string(animPath != nullptr ? animPath : ""));
		if (!data.has_value())
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: file not found: {}", animPath != nullptr ? animPath : "");
			return -1;
		}

#pragma pack(push, 1)

		struct V1Header
		{
			char magic[4];
			std::uint32_t version;
			std::uint32_t channelCount;
			std::uint16_t nameLen;
		};

#pragma pack(pop)

		aether::BinaryReader reader(*data);
		auto v1Hdr = reader.Read<V1Header>();
		if (v1Hdr.magic[0] != 'A' || v1Hdr.magic[1] != 'N' || v1Hdr.magic[2] != 'I' || v1Hdr.magic[3] != 'M')
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: invalid animation magic");
			return -1;
		}

		const std::uint32_t version = v1Hdr.version;
		const std::uint32_t channelCount = v1Hdr.channelCount;
		const std::uint16_t nameLen = v1Hdr.nameLen;
		std::uint16_t flags = 0;
		if (version >= 2)
		{
			flags = reader.Read<std::uint16_t>();
		}

		aether::assets::GltfAnimation anim;
		anim.name = std::string(reinterpret_cast<const char*>(reader.Data()), nameLen);
		reader.Advance(nameLen);

		const bool hasBoneNames = (version >= 2) && ((flags & 1) != 0);

		for (std::uint32_t ci = 0; ci < channelCount; ++ci)
		{
			aether::assets::GltfAnimationChannel ch;
			auto diskCh = reader.Read<ChannelHeaderDisk>();
			ch.nodeIndex = diskCh.nodeIndex;

			if (hasBoneNames)
			{
				auto boneNameLen = reader.Read<std::uint16_t>();
				if (boneNameLen > 0)
				{
					ch.boneName = std::string(reinterpret_cast<const char*>(reader.Data()), boneNameLen);
					reader.Advance(boneNameLen);
				}
			}

			switch (static_cast<AnimPathDisk>(diskCh.path))
			{
				case AnimPathDisk::Translation:
					ch.path = aether::assets::GltfAnimationPath::Translation;
					break;
				case AnimPathDisk::Rotation:
					ch.path = aether::assets::GltfAnimationPath::Rotation;
					break;
				case AnimPathDisk::Scale:
					ch.path = aether::assets::GltfAnimationPath::Scale;
					break;
				case AnimPathDisk::Weights:
					ch.path = aether::assets::GltfAnimationPath::Weights;
					break;
			}

			switch (static_cast<AnimInterpDisk>(diskCh.interp))
			{
				case AnimInterpDisk::Linear:
					ch.interpolation = aether::assets::GltfInterpolation::Linear;
					break;
				case AnimInterpDisk::Step:
					ch.interpolation = aether::assets::GltfInterpolation::Step;
					break;
				case AnimInterpDisk::CubicSpline:
					ch.interpolation = aether::assets::GltfInterpolation::CubicSpline;
					break;
			}

			ch.times.resize(diskCh.keyCount);
			reader.ReadRaw(ch.times.data(), diskCh.keyCount * sizeof(float));

			ch.values.resize(diskCh.keyCount);
			for (std::uint32_t k = 0; k < diskCh.keyCount; ++k)
			{
				float v4[4];
				reader.ReadRaw(v4, sizeof(v4));
				ch.values[k] = glm::vec4(v4[0], v4[1], v4[2], v4[3]);
			}

			anim.channels.push_back(std::move(ch));
		}

		auto boneMap = BuildBoneNameMap(smc->animDb);
		RemapAnimationByBoneName(anim, boneMap, smc->animDb);
		anim.rootLocked = lockRoot;

		const auto pendingIdx = static_cast<std::uint32_t>(smc->pendingExternalAnims.size());
		smc->pendingExternalAnims.push_back(std::move(anim));

		const std::uint32_t internalClipCount = smc->animDb != nullptr ? smc->animDb->GetClipCount() : 0;
		return static_cast<std::int32_t>(internalClipCount + pendingIdx);
	}
#else
	// The browser has no AnimationDatabase: external animation files cannot be compiled into
	// one, so loading them is an explicit unsupported operation rather than a silent no-op.
	std::int32_t AddAnimationImpl(aether::World&, std::uint32_t id, const char* animPath, bool)
	{
		AE_ERROR(aether::LogCategory::Animation, "WEB_FEATURE_UNSUPPORTED: Animation.Add/LoadExternal('{}') on entity {}: external animation files need the native animation database", animPath != nullptr ? animPath : "", id);
		return -1;
	}
#endif
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
	(void) ctx;
	AE_ERROR(aether::LogCategory::Animation, "WEB_FEATURE_UNSUPPORTED: Animation.Compile on entity {}: external animation files need the native animation database", id);
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
	// Joint offsets address skeleton nodes through the native animation database.
	AE_ERROR(aether::LogCategory::Animation, "WEB_FEATURE_UNSUPPORTED: Animation.SetJointOffset('{}') on entity {}: joint offsets need the native animation database", jointName, id);
	return;
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
