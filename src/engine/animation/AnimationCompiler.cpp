#include "animation/AnimationCompiler.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "animation/AnimationDatabase.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		static VkCommandPool s_uploadPool = VK_NULL_HANDLE;
	} // namespace

	void SetAnimationCompilePool(VkCommandPool pool)
	{
		s_uploadPool = pool;
	}

	void CompileAnimations(World& world, std::uint32_t entityId)
	{
		if (s_uploadPool == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Animation, "CompileAnimations: no upload pool set - call SetAnimationCompilePool during app init");
			return;
		}

		const auto* sec = world.TryGet<SpawnedEntitiesComponent>(Entity{entityId});

		auto compileOne = [](SkinnedMeshComponent& smc)
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

			std::vector<AnimationDatabase::GpuClip> clips;
			std::vector<AnimationDatabase::GpuChannel> channels;
			std::vector<float> times;
			std::vector<glm::vec4> values;
			std::string clipNames;

			for (const auto& anim: smc.pendingExternalAnims)
			{
				const std::uint32_t nameOff = static_cast<std::uint32_t>(clipNames.size());
				clipNames += anim.name;

				AnimationDatabase::GpuClip clip{};
				clip.nameOffset = nameOff;
				clip.nameLength = static_cast<std::uint32_t>(anim.name.size());
				clip.channelOffset = static_cast<std::uint32_t>(channels.size());
				clip.channelCount = static_cast<std::uint32_t>(anim.channels.size());
				clip.duration = 0.f;

				uint32_t skippedChannels = 0;
				for (const auto& ch: anim.channels)
				{
					if (ch.nodeIndex >= nodeCount)
					{
						++skippedChannels;
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
				if (skippedChannels > 0)
				{
					AE_WARN(aether::LogCategory::Animation, "CompileAnimations: skipped {}/{} channels for clip '{}' (nodeIndex >= nodeCount={})", skippedChannels, anim.channels.size(), anim.name, nodeCount);
				}

				clips.push_back(clip);
			}

			auto result = smc.animDb->AppendAnimations(s_uploadPool, clips, channels, times, values, clipNames);
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

		if (auto* smc = world.TryGet<SkinnedMeshComponent>(Entity{entityId}))
		{
			compileOne(*smc);
		}

		if (sec)
		{
			for (const auto eid: sec->entityIds)
			{
				if (auto* smc = world.TryGet<SkinnedMeshComponent>(Entity{eid}))
				{
					compileOne(*smc);
				}
			}
		}
	}
} // namespace aether
