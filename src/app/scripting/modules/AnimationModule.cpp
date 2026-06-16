#include "scripting/DasModuleBase.hpp"

#include <algorithm>
#include <unordered_map>

#include "daScript/daScript.h"

#include "BinaryFormats.hpp"
#include "assets/GltfAsset.hpp"
#include "animation/AnimationCompiler.hpp"
#include "animation/AnimationDatabase.hpp"
#include "animation/AnimationIk.hpp"
#include "io/FileSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/BinaryReader.hpp"

namespace
{
	using namespace aether::app::scripting;

	aether::AnimationIkSystem* s_animationIkSystem = nullptr;

	// -- Helpers ---------------------------------------------------------------

	const aether::SkinnedMeshComponent* FindSmcOrSpawned(const aether::World* w, uint32_t id)
	{
		const aether::Entity e{id};
		if (const auto smc = w->TryGet<aether::SkinnedMeshComponent>(e))
		{
			return smc;
		}
		const auto sec = w->TryGet<aether::SpawnedEntitiesComponent>(e);
		if (sec && !sec->entityIds.empty())
		{
			return w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{sec->entityIds.front()});
		}
		return nullptr;
	}

	aether::SkinnedMeshComponent* FindSmcOrSpawned(aether::World* w, uint32_t id)
	{
		const aether::Entity e{id};
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(e))
		{
			return smc;
		}
		const auto sec = w->TryGet<aether::SpawnedEntitiesComponent>(e);
		if (sec && !sec->entityIds.empty())
		{
			return w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{sec->entityIds.front()});
		}
		return nullptr;
	}

	void ForEachSpawnedSmc(aether::World* w, uint32_t id, auto&& f)
	{
		const auto sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (!sec)
		{
			return;
		}
		for (const auto eid: sec->entityIds)
		{
			if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{eid}))
			{
				f(*smc);
			}
		}
	}

	// Strip known skeleton prefixes from a bone name, returning the cleaned name.
	// Returns empty string if the input is empty.
	static std::string StripBonePrefix(std::string name)
	{
		static constexpr const char* kPrefixes[] = {"mixamorig:", "mixamorig_", "Armature_"};
		for (const auto prefix: kPrefixes)
		{
			const std::size_t plen = std::strlen(prefix);
			if (name.size() > plen && name.substr(0, plen) == prefix)
			{
				return name.substr(plen);
			}
		}
		return name;
	}

	// Build bone name -> node index map from AnimationDatabase's skeleton.
	// Registers:
	//   - The full skeleton name (e.g. "mixamorig_Hips" -> nodeIdx)
	//   - The stripped name  (e.g. "Hips" -> same nodeIdx)
	//   - Alternate prefix forms  (e.g. "mixamorig:Hips" -> same nodeIdx)
	std::unordered_map<std::string, std::uint32_t> BuildBoneNameMap(const aether::AnimationDatabase* animDb)
	{
		std::unordered_map<std::string, std::uint32_t> boneMap;
		if (!animDb)
		{
			return boneMap;
		}

		const auto skinCount = animDb->GetSkinCount();
		if (skinCount == 0)
		{
			return boneMap;
		}

		const auto jointCount = animDb->GetSkinJointCount(0);
		const auto& skinJoints = animDb->GetSkinJoints();

		static constexpr const char* kAltPrefixes[] = {"mixamorig:", "mixamorig_", "Armature_"};

		for (std::uint32_t i = 0; i < jointCount; ++i)
		{
			const auto nodeIdx = skinJoints[i];
			std::string nodeName(animDb->GetNodeName(nodeIdx));
			if (nodeName.empty())
			{
				continue;
			}

			// Full name.
			boneMap[nodeName] = nodeIdx;

			// Stripped name (without the leading prefix).
			std::string stripped = StripBonePrefix(nodeName);
			if (stripped != nodeName)
			{
				boneMap.try_emplace(stripped, nodeIdx);
			}

			// Alternate prefix forms: if the skeleton has "mixamorig_Hips",
			// also register "mixamorig:Hips" and "Armature_Hips".
			if (!stripped.empty())
			{
				for (const auto altPrefix: kAltPrefixes)
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

	// Remap animation channels to the target skeleton.
	// Tries bone-name matching first (stripping prefixes from both sides).
	// If no channels have bone names, falls back to index-ordering matching.
	void RemapAnimationByBoneName(aether::assets::GltfAnimation& anim, const std::unordered_map<std::string, std::uint32_t>& boneNameToJointIndex, const aether::AnimationDatabase* animDb)
	{
		bool hasBoneNames = false;
		for (auto& ch: anim.channels)
		{
			if (!ch.boneName.empty())
			{
				hasBoneNames = true;

				// Try direct lookup first.
				auto it = boneNameToJointIndex.find(ch.boneName);
				if (it != boneNameToJointIndex.end())
				{
					ch.nodeIndex = it->second;
					continue;
				}

				// Try stripping prefix from the .anim's bone name too,
				// in case the map has the full name but the file has prefixed.
				std::string animBare = StripBonePrefix(ch.boneName);
				if (animBare != ch.boneName)
				{
					it = boneNameToJointIndex.find(animBare);
					if (it != boneNameToJointIndex.end())
					{
						ch.nodeIndex = it->second;
					}
				}
			}
		}

		// Fallback: if the .anim file has no bone names, match by index ordering.
		if (!hasBoneNames && animDb)
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

	// -- Compile ---------------------------------------------------------------

	// Bake all pending animation clips into the model's AnimationDatabase.
	// Delegates to the engine-level CompileAnimations.
	void das_compile_animations(aether::World* w, uint32_t id)
	{
		aether::CompileAnimations(*w, id);
	}

	// -- Loading ---------------------------------------------------------------

	// Forward declaration for the wrapper below.
	int32_t das_add_animation(aether::World* w, uint32_t id, const char* animPath, bool lockRoot = false);

	// Backward-compatible 3-arg wrapper for load_external_animation.
	int32_t das_load_external_animation(aether::World* w, uint32_t id, const char* animPath)
	{
		return das_add_animation(w, id, animPath, false);
	}

	// Load a .anim file and add it as a pending clip.
	// Returns the future clip index (valid after compile_animations).
	// If lockRoot is true, root bone translation channels are stripped
	// during compilation (prevents root motion like walking in place).
	int32_t das_add_animation(aether::World* w, uint32_t id, const char* animPath, bool lockRoot)
	{
		auto smc = FindSmcOrSpawned(w, id);
		if (!smc)
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: entity {} has no SkinnedMeshComponent", id);
			return -1;
		}

		auto data = aether::io::FileSystem::ReadFile(std::string(animPath));
		if (!data.has_value())
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: file not found: {}", animPath);
			return -1;
		}

#pragma pack(push, 1)

		struct V1Header
		{
			char magic[4];
			uint32_t version;
			uint32_t channelCount;
			uint16_t nameLen;
		};

#pragma pack(pop)

		aether::BinaryReader reader(*data);

		auto v1Hdr = reader.Read<V1Header>();
		if (v1Hdr.magic[0] != 'A' || v1Hdr.magic[1] != 'N' || v1Hdr.magic[2] != 'I' || v1Hdr.magic[3] != 'M')
		{
			AE_WARN(aether::LogCategory::Animation, "add_animation: invalid animation magic");
			return -1;
		}

		uint32_t version = v1Hdr.version;
		uint32_t channelCount = v1Hdr.channelCount;
		uint16_t nameLen = v1Hdr.nameLen;
		uint16_t flags = 0;
		if (version >= 2)
		{
			flags = reader.Read<uint16_t>();
		}

		aether::assets::GltfAnimation anim;
		anim.name = std::string(reinterpret_cast<const char*>(reader.Data()), nameLen);
		reader.Advance(nameLen);

		const bool hasBoneNames = (version >= 2) && (flags & 1);
		AE_VERBOSE(aether::LogCategory::Animation, "add_animation: .anim v{}, {} channels, name='{}', hasBoneNames={}", version, channelCount, anim.name.c_str(), hasBoneNames);

		for (uint32_t ci = 0; ci < channelCount; ++ci)
		{
			aether::assets::GltfAnimationChannel ch;

			// The on-disk format writes ChannelHeaderDisk FIRST (nodeIndex + path +
			// interp + padding + keyCount = 12 bytes), THEN the optional bone name.
			auto diskCh = reader.Read<ChannelHeaderDisk>();
			ch.nodeIndex = diskCh.nodeIndex;

			if (hasBoneNames)
			{
				auto boneNameLen = reader.Read<uint16_t>();
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
			for (uint32_t k = 0; k < diskCh.keyCount; ++k)
			{
				float v4[4];
				reader.ReadRaw(v4, sizeof(v4));
				ch.values[k] = glm::vec4(v4[0], v4[1], v4[2], v4[3]);
			}

			anim.channels.push_back(ch);
		}

		{
			std::size_t nonEmpty = 0;
			for (const auto& ch: anim.channels)
			{
				if (!ch.boneName.empty())
				{
					if (nonEmpty < 5)
					{
						AE_VERBOSE(aether::LogCategory::Animation, "  channel boneName[{}]: '{}'", nonEmpty, ch.boneName);
					}
					++nonEmpty;
				}
			}
			AE_VERBOSE(aether::LogCategory::Animation, "add_animation: {}/{} channels have non-empty boneName", nonEmpty, anim.channels.size());
		}

		auto boneMap = BuildBoneNameMap(smc->animDb);
		RemapAnimationByBoneName(anim, boneMap, smc->animDb);

		{
			const std::uint32_t nodeCount = smc->animDb ? smc->animDb->GetNodeCount() : 0;
			uint32_t matched = 0, unmatched = 0, oob = 0;
			for (const auto& ch: anim.channels)
			{
				if (!ch.boneName.empty())
				{
					if (boneMap.count(ch.boneName) > 0)
					{
						++matched;
					}
					else
					{
						++unmatched;
					}
				}
				if (ch.nodeIndex >= nodeCount)
				{
					++oob;
				}
			}
			if (unmatched > 0)
			{
				AE_WARN(aether::LogCategory::Animation, "add_animation: {}/{} channels had unmatched bone names in skeleton", unmatched, unmatched + matched);
			}
			if (oob > 0)
			{
				AE_WARN(aether::LogCategory::Animation, "add_animation: {} channels have nodeIndex >= nodeCount({}) after remap - will be skipped on GPU", oob, nodeCount);
			}
			AE_INFO(aether::LogCategory::Animation, "add_animation: {} channels, {} matched by name, {} total nodes in skeleton", anim.channels.size(), matched, nodeCount);
		}

		anim.rootLocked = lockRoot;

		const auto pendingIdx = static_cast<std::uint32_t>(smc->pendingExternalAnims.size());
		smc->pendingExternalAnims.push_back(std::move(anim));

		const std::uint32_t internalClipCount = smc->animDb ? smc->animDb->GetClipCount() : 0;
		const std::uint32_t futureClipIndex = internalClipCount + pendingIdx;

		AE_VERBOSE(aether::LogCategory::Animation, "add_animation: loaded '{}' for entity {}, future clip index={}, rootLocked={}", smc->pendingExternalAnims.back().name, id, futureClipIndex, lockRoot);

		return static_cast<int32_t>(futureClipIndex);
	}

	void das_clear_pending_animations(aether::World* w, uint32_t id)
	{
		auto clearFn = [](aether::SkinnedMeshComponent& smc)
		{
			smc.pendingExternalAnims.clear();
		};
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			clearFn(*smc);
		}
		ForEachSpawnedSmc(w, id, clearFn);
	}

	// -- Playback control ------------------------------------------------------

	void das_set_animation(aether::World* w, uint32_t id, int32_t clipIndex)
	{
		if (clipIndex < 0)
		{
			return;
		}
		const auto uIdx = static_cast<std::uint32_t>(clipIndex);
		auto setClip = [uIdx](aether::SkinnedMeshComponent& smc)
		{
			smc.clipIndex = uIdx;
			smc.animTime = 0.f;
		};
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			setClip(*smc);
		}
		ForEachSpawnedSmc(w, id, setClip);
	}

	int32_t das_get_current_animation(aether::World* w, uint32_t id)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		return smc ? static_cast<int32_t>(smc->clipIndex) : -1;
	}

	void das_set_playback_speed(aether::World* w, uint32_t id, float speed)
	{
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			smc->playbackSpeed = speed;
		}
		ForEachSpawnedSmc(w, id, [speed](aether::SkinnedMeshComponent& smc) { smc.playbackSpeed = speed; });
	}

	float das_get_playback_speed(aether::World* w, uint32_t id)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		return smc ? smc->playbackSpeed : 0.f;
	}

	void das_set_anim_time(aether::World* w, uint32_t id, float t)
	{
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(aether::Entity{id}))
		{
			smc->animTime = t;
		}
		ForEachSpawnedSmc(w, id, [t](aether::SkinnedMeshComponent& smc) { smc.animTime = t; });
	}

	float das_get_anim_time(aether::World* w, uint32_t id)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		return smc ? smc->animTime : 0.f;
	}

	// -- Clip queries (checks both compiled DB clips and pending clips) ---------

	int32_t das_get_animation_count(aether::World* w, uint32_t id)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		if (!smc)
		{
			return 0;
		}
		const auto dbCount = smc->animDb ? smc->animDb->GetClipCount() : 0;
		return static_cast<int32_t>(dbCount + smc->pendingExternalAnims.size());
	}

	const char* das_get_animation_name(aether::World* w, uint32_t id, int32_t index)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		if (!smc || index < 0)
		{
			return nullptr;
		}
		const auto uIdx = static_cast<std::uint32_t>(index);
		const auto dbCount = smc->animDb ? smc->animDb->GetClipCount() : 0;

		if (uIdx < dbCount)
		{
			return smc->animDb->GetClipName(uIdx).data();
		}
		const auto pendingIdx = uIdx - dbCount;
		if (pendingIdx < smc->pendingExternalAnims.size())
		{
			return smc->pendingExternalAnims[pendingIdx].name.c_str();
		}
		return nullptr;
	}

	float das_get_animation_duration(aether::World* w, uint32_t id)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		if (!smc)
		{
			return 0.f;
		}
		const auto clipIdx = smc->clipIndex;
		const auto dbCount = smc->animDb ? smc->animDb->GetClipCount() : 0;

		if (clipIdx < dbCount)
		{
			return smc->animDb->GetClipDuration(clipIdx);
		}
		const auto pendingIdx = clipIdx - dbCount;
		if (pendingIdx < smc->pendingExternalAnims.size())
		{
			float dur = 0.f;
			for (const auto& ch: smc->pendingExternalAnims[pendingIdx].channels)
			{
				if (!ch.times.empty())
				{
					dur = std::max(dur, ch.times.back());
				}
			}
			return dur;
		}
		return 0.f;
	}

	int32_t das_find_animation(aether::World* w, uint32_t id, const char* name)
	{
		const auto smc = FindSmcOrSpawned(w, id);
		if (!smc || !name)
		{
			return -1;
		}
		const auto dbCount = smc->animDb ? smc->animDb->GetClipCount() : 0;

		for (std::uint32_t i = 0; i < dbCount; ++i)
		{
			if (smc->animDb->GetClipName(i) == name)
			{
				return static_cast<int32_t>(i);
			}
		}
		for (std::uint32_t i = 0; i < smc->pendingExternalAnims.size(); ++i)
		{
			if (smc->pendingExternalAnims[i].name == name)
			{
				return static_cast<int32_t>(dbCount + i);
			}
		}
		return -1;
	}

	// -- Entity iteration ------------------------------------------------------

	void das_for_each_with_animator(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		for (auto enttE: w->View<aether::SkinnedMeshComponent>())
		{
			const auto eid = static_cast<uint32_t>(entt::to_integral(enttE));
			vec4f args[1];
			args[0] = das::cast<uint32_t>::from(eid);
			ctx->invoke(block, args, nullptr, at);
		}
	}

	// -- Animation blend ------------------------------------------------------

	void das_set_animation_blend(aether::World* w, uint32_t id, int32_t secondaryClipIndex, float transitionSpeed)
	{
		if (secondaryClipIndex < 0)
		{
			return;
		}
		const auto uIdx = static_cast<std::uint32_t>(secondaryClipIndex);
		float speed = transitionSpeed > 0.f ? transitionSpeed : 4.0f;

		const aether::Entity entity{id};
		if (auto smc = w->TryGet<aether::SkinnedMeshComponent>(entity))
		{
			auto& blend = w->GetRegistry().get_or_emplace<aether::AnimationBlendComponent>(aether::World::ToEntt(entity));
			blend.secondaryClip = uIdx;
			blend.blendWeight = 1.0f;
			blend.transitionSpeed = speed;
			blend.inTransition = true;
		}
		ForEachSpawnedSmc(w,
		        id,
		        [&](aether::SkinnedMeshComponent&)
		        {
			        const aether::Entity spawnedEntity{id};
			        auto& blend = w->GetRegistry().get_or_emplace<aether::AnimationBlendComponent>(aether::World::ToEntt(spawnedEntity));
			        blend.secondaryClip = uIdx;
			        blend.blendWeight = 1.0f;
			        blend.transitionSpeed = speed;
			        blend.inTransition = true;
		        });
	}

	// -- IK -------------------------------------------------------------------

	void das_set_ik_enabled(aether::World* w, uint32_t id, bool enabled)
	{
		const aether::Entity entity{id};
		if (auto ikComp = w->TryGet<aether::IkTargetsComponent>(entity))
		{
			ikComp->enabled = enabled;
		}
		ForEachSpawnedSmc(w,
		        id,
		        [&](aether::SkinnedMeshComponent&)
		        {
			        if (auto comp = w->TryGet<aether::IkTargetsComponent>(aether::Entity{id}))
			        {
				        comp->enabled = enabled;
			        }
		        });
	}

	bool das_get_foot_contact(aether::World* w, uint32_t id, int32_t footIndex)
	{
		const aether::Entity entity{id};
		const auto ikComp = w->TryGet<aether::IkTargetsComponent>(entity);
		if (!ikComp)
		{
			return false;
		}
		return footIndex == 0 ? ikComp->leftFootPlanted : ikComp->rightFootPlanted;
	}

	float das_get_foot_offset_y(aether::World* w, uint32_t id, int32_t footIndex)
	{
		const aether::Entity entity{id};
		const auto ikComp = w->TryGet<aether::IkTargetsComponent>(entity);
		if (!ikComp)
		{
			return 0.f;
		}
		return footIndex == 0 ? ikComp->leftFootOffset.y : ikComp->rightFootOffset.y;
	}

	// init_entity_ik(world, entity_id)
	// Initializes IK bone indices and leg lengths for an entity after animation compilation.
	void das_init_entity_ik(aether::World* w, uint32_t id)
	{
		if (!s_animationIkSystem)
		{
			return;
		}
		auto smc = FindSmcOrSpawned(w, id);
		if (!smc || !smc->animDb)
		{
			return;
		}
		s_animationIkSystem->InitEntity(*w, id, *smc->animDb);
	}

	// -- Root motion ----------------------------------------------------------

	void das_set_root_motion_enabled(aether::World* w, uint32_t id, bool enabled)
	{
		const aether::Entity entity{id};
		if (auto rmComp = w->TryGet<aether::RootMotionComponent>(entity))
		{
			rmComp->enabled = enabled;
		}
		ForEachSpawnedSmc(w,
		        id,
		        [&](aether::SkinnedMeshComponent&)
		        {
			        if (auto comp = w->TryGet<aether::RootMotionComponent>(aether::Entity{id}))
			        {
				        comp->enabled = enabled;
			        }
		        });
	}

	bool das_get_root_motion_enabled(aether::World* w, uint32_t id)
	{
		const aether::Entity entity{id};
		const auto rmComp = w->TryGet<aether::RootMotionComponent>(entity);
		return rmComp ? rmComp->enabled : false;
	}

	void das_get_root_motion_delta(aether::World* w, uint32_t id, float& outX, float& outY, float& outZ)
	{
		const aether::Entity entity{id};
		const auto rmComp = w->TryGet<aether::RootMotionComponent>(entity);
		if (rmComp)
		{
			outX = rmComp->accumulatedDelta.x;
			outY = rmComp->accumulatedDelta.y;
			outZ = rmComp->accumulatedDelta.z;
		}
		else
		{
			outX = outY = outZ = 0.f;
		}
	}

} // namespace

namespace aether::app::scripting
{
	void InitAnimationModule(aether::AnimationIkSystem* ik)
	{
		s_animationIkSystem = ik;
	}

	struct AnimationModule : DasModuleBase
	{
		AnimationModule()
		      : DasModuleBase("animation")
		{
			das::ModuleLibrary lib(this);

			// Compile - bake pending animations into the AnimationDatabase
			Bind<das_compile_animations>(lib, "compile_animations", SE::modifyExternal);

			// Loading
			Bind<das_add_animation>(lib, "add_animation", SE::modifyExternal);
			Bind<das_load_external_animation>(lib, "load_external_animation", SE::modifyExternal);
			Bind<das_clear_pending_animations>(lib, "clear_pending_animations", SE::modifyExternal);

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

			// Blend
			Bind<das_set_animation_blend>(lib, "set_animation_blend", SE::modifyExternal);

			// IK
			Bind<das_set_ik_enabled>(lib, "set_ik_enabled", SE::modifyExternal);
			Bind<das_get_foot_contact>(lib, "get_foot_contact", SE::accessExternal);
			Bind<das_get_foot_offset_y>(lib, "get_foot_offset_y", SE::accessExternal);
			Bind<das_init_entity_ik>(lib, "init_entity_ik", SE::modifyExternal);

			// Root motion
			Bind<das_set_root_motion_enabled>(lib, "set_root_motion_enabled", SE::modifyExternal);
			Bind<das_get_root_motion_enabled>(lib, "get_root_motion_enabled", SE::accessExternal);
			Bind<das_get_root_motion_delta>(lib, "get_root_motion_delta", SE::accessExternal);

			// Entity iteration (name kept for script backward compatibility)
			Bind<das_for_each_with_animator>(lib, "for_each_with_animator", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(AnimationModule, aether::app::scripting)
