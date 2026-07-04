#include "animation/AnimationCompiler.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "animation/AnimationDatabase.hpp"
#include "assets/GltfAsset.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void CompileAnimations(World& world, std::uint32_t entityId, gpu::CommandPool uploadPool)
	{
		AE_PROFILE_ZONE();
		if (uploadPool == nullptr)
		{
			AE_WARN(LogCategory::Animation, "CompileAnimations: upload pool is null");
			return;
		}

		const auto hier = world.TryGet<HierarchyComponent>(Entity{entityId});

		auto compileOne = [&uploadPool](SkinnedMeshComponent& smc)
		{
			if (smc.pendingExternalAnims.empty() || !smc.animDb)
			{
				return;
			}

			const std::uint32_t nodeCount = smc.animDb->GetNodeCount();
			if (nodeCount == 0)
			{
				return;
			}

			// Cache root bones once for all clips that need locking.
			struct RootLockInfo
			{
				bool any = false;
				std::vector<std::uint32_t> boneIndices;
			};
			RootLockInfo rootInfo;

			const auto translationPath = static_cast<std::uint8_t>(aether::assets::GltfAnimationPath::Translation);

			std::vector<AnimationDatabase::GpuClip> clips;
			std::vector<AnimationDatabase::GpuChannel> channels;
			std::vector<float> times;
			std::vector<glm::vec4> values;
			std::string clipNames;

			for (const auto& anim: smc.pendingExternalAnims)
			{
				// Lazily build root bone list on first clip that needs it.
				if (anim.rootLocked && !rootInfo.any)
				{
					const auto& parents = smc.animDb->GetNodeParents();
					for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(parents.size()); ++i)
					{
						if (parents[i] == 0)
						{
							rootInfo.boneIndices.push_back(i);
						}
					}
					rootInfo.any = true;
					if (!rootInfo.boneIndices.empty())
					{
						AE_VERBOSE(LogCategory::Animation, "CompileAnimations: locking root translation for {} bone(s)", rootInfo.boneIndices.size());
					}
				}

				const auto nameOff = static_cast<std::uint32_t>(clipNames.size());
				clipNames += anim.name;

				AnimationDatabase::GpuClip clip{};
				clip.nameOffset = nameOff;
				clip.nameLength = static_cast<std::uint32_t>(anim.name.size());
				clip.channelOffset = static_cast<std::uint32_t>(channels.size());
				clip.channelCount = static_cast<std::uint32_t>(anim.channels.size());
				clip.duration = 0.f;

				uint32_t skippedChannels = 0;
				uint32_t lockedChannels = 0;
				for (const auto& ch: anim.channels)
				{
					if (ch.nodeIndex >= nodeCount)
					{
						++skippedChannels;
						continue;
					}

					// Skip translation channels for root bones when root lock is active.
					const bool isRootTranslation = anim.rootLocked && static_cast<std::uint8_t>(ch.path) == translationPath && std::ranges::find(rootInfo.boneIndices, ch.nodeIndex) != rootInfo.boneIndices.end();
					if (isRootTranslation)
					{
						++lockedChannels;
						continue;
					}

					AnimationDatabase::GpuChannel gpuCh{};
					gpuCh.nodeIndex = ch.nodeIndex;
					gpuCh.animPath = static_cast<std::uint8_t>(ch.path);
					gpuCh.interpolation = static_cast<std::uint8_t>(ch.interpolation);
					gpuCh.timesOffset = static_cast<std::uint32_t>(times.size() * sizeof(float));
					gpuCh.timesCount = static_cast<std::uint32_t>(ch.times.size());
					times.insert(times.end(), ch.times.begin(), ch.times.end());

					if (!ch.times.empty())
					{
						clip.duration = std::max(clip.duration, ch.times.back());
					}

					gpuCh.valuesOffset = static_cast<std::uint32_t>(values.size() * sizeof(glm::vec4));
					gpuCh.valuesCount = static_cast<std::uint32_t>(ch.values.size());
					values.insert(values.end(), ch.values.begin(), ch.values.end());

					channels.push_back(gpuCh);
				}
				clip.channelCount = static_cast<std::uint32_t>(channels.size()) - clip.channelOffset;
				if (skippedChannels > 0)
				{
					AE_WARN(aether::LogCategory::Animation, "CompileAnimations: skipped {}/{} channels for clip '{}' (nodeIndex >= nodeCount={})", skippedChannels, anim.channels.size(), anim.name, nodeCount);
				}
				if (lockedChannels > 0)
				{
					AE_VERBOSE(aether::LogCategory::Animation, "CompileAnimations: locked {} translation channel(s) for clip '{}'", lockedChannels, anim.name);
				}

				clips.push_back(clip);
			}

			auto result = smc.animDb->AppendAnimations(uploadPool, clips, channels, times, values, clipNames);
			if (result)
			{
				AE_VERBOSE(LogCategory::Animation, "CompileAnimations: baked {} clip(s) into DB (first at index {})", clips.size(), *result);
			}
			else
			{
				AE_ERROR(LogCategory::Animation, "CompileAnimations: AppendAnimations failed");
			}

			smc.pendingExternalAnims.clear();
		};

		if (auto smc = world.TryGet<SkinnedMeshComponent>(Entity{entityId}))
		{
			compileOne(*smc);
		}

		if (hier)
		{
			for (const Entity child: hier->children)
			{
				if (auto smc = world.TryGet<SkinnedMeshComponent>(child))
				{
					compileOne(*smc);
				}
			}
		}
	}
} // namespace aether
