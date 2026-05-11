#include "animation/AnimationSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "animation/ModelAnimator.hpp"
#include "utils/Profiler.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		struct PackedAnimatorState
		{
			ModelAnimator* animator = nullptr;
			bool active = false;
			bool hero = true;
			std::uint8_t lodTier = 0;
			std::uint8_t updateIntervalFrames = 1;
			std::uint32_t updatePhaseSeed = 0;
			float pendingDt = 0.0f;
		};

		std::vector<PackedAnimatorState> g_packedAnimators;
		std::unordered_map<ModelAnimator*, std::uint32_t> g_animatorToPacked;
		std::uint32_t g_frameCounter = 0;
		std::uint32_t g_lodUpdateCursor[4] = { 0, 0, 0, 0 };
		float g_lodUpdateCarry[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		std::uint32_t MakePhaseSeed(const ModelAnimator* animator)
		{
			std::uintptr_t x = reinterpret_cast<std::uintptr_t>(animator);
			x ^= (x >> 17);
			x *= 0xed5ad4bbU;
			x ^= (x >> 11);
			x *= 0xac4c1b51U;
			x ^= (x >> 15);
			return static_cast<std::uint32_t>(x);
		}

		std::uint8_t ComputeDistanceLod(const glm::mat4& localToWorld)
		{
			const glm::vec3 p = glm::vec3(localToWorld[3]);
			const float dist = std::sqrt(glm::dot(p, p));
			if (dist < 8.0f)
			{
				return 0;
			}
			if (dist < 18.0f)
			{
				return 1;
			}
			if (dist < 35.0f)
			{
				return 2;
			}
			return 3;
		}

		std::uint8_t IntervalForLod(std::uint8_t lod)
		{
			switch (lod)
			{
				case 0:
					return 1;
				case 1:
					return 2;
				case 2:
					return 4;
				default:
					return 8;
			}
		}
	} // namespace

	void AnimationSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE_N("AnimationSystem.Update");
		++g_frameCounter;

		for (PackedAnimatorState& p: g_packedAnimators)
		{
			p.active = false;
		}

		auto view = world.View<AnimatorComponent, TransformComponent>();
		std::uint32_t animatorCountByLod[4] = { 0, 0, 0, 0 };
		std::uint32_t heroCount = 0;

		for (auto entity: view)
		{
			(void) entity;
			auto& animatorComp = view.get<AnimatorComponent>(entity);
			const auto& transform = view.get<TransformComponent>(entity);
			if (animatorComp.animator == nullptr)
			{
				continue;
			}

			auto it = g_animatorToPacked.find(animatorComp.animator);
			std::uint32_t packedIndex = 0;
			if (it == g_animatorToPacked.end())
			{
				packedIndex = static_cast<std::uint32_t>(g_packedAnimators.size());
				g_animatorToPacked.emplace(animatorComp.animator, packedIndex);
				g_packedAnimators.push_back(PackedAnimatorState{
				        .animator = animatorComp.animator,
				        .updatePhaseSeed = MakePhaseSeed(animatorComp.animator),
				});
			}
			else
			{
				packedIndex = it->second;
			}

			PackedAnimatorState& packed = g_packedAnimators[packedIndex];
			const std::uint8_t distanceLod = ComputeDistanceLod(transform.localToWorld);
			const std::uint8_t requestedLod = animatorComp.heroCharacter ? 0 : std::max(animatorComp.lodTier, distanceLod);

			if (!packed.active)
			{
				packed.active = true;
				packed.hero = animatorComp.heroCharacter;
				packed.lodTier = requestedLod;
				packed.updateIntervalFrames = IntervalForLod(requestedLod);
				packed.pendingDt += dt;
			}
			else
			{
				packed.hero = packed.hero || animatorComp.heroCharacter;
				packed.lodTier = std::min(packed.lodTier, requestedLod);
				packed.updateIntervalFrames = IntervalForLod(packed.lodTier);
			}

			// Track LOD distribution for telemetry.
			if (packed.hero)
			{
				++heroCount;
			}
			else if (packed.lodTier < 4)
			{
				++animatorCountByLod[packed.lodTier];
			}
		}

		std::uint32_t updatedThisFrame = 0;
		std::vector<std::uint32_t> lodBuckets[4];

		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(g_packedAnimators.size()); ++i)
		{
			PackedAnimatorState& packed = g_packedAnimators[i];
			if (!packed.active || packed.animator == nullptr)
			{
				continue;
			}
			if (packed.pendingDt <= 0.0f)
			{
				continue;
			}

			if (packed.hero)
			{
				packed.animator->Update(packed.pendingDt);
				packed.pendingDt = 0.0f;
				++updatedThisFrame;
				continue;
			}

			const std::uint32_t lod = std::min<std::uint32_t>(packed.lodTier, 3u);
			lodBuckets[lod].push_back(i);
		}

		for (std::uint32_t lod = 0; lod < 4; ++lod)
		{
			auto& bucket = lodBuckets[lod];
			if (bucket.empty())
			{
				continue;
			}

			const std::uint32_t interval = std::max<std::uint32_t>(1u, IntervalForLod(static_cast<std::uint8_t>(lod)));
			if (interval <= 1u)
			{
				for (const std::uint32_t idx: bucket)
				{
					PackedAnimatorState& packed = g_packedAnimators[idx];
					packed.animator->Update(packed.pendingDt);
					packed.pendingDt = 0.0f;
					++updatedThisFrame;
				}
				continue;
			}

			const float targetPerFrame = static_cast<float>(bucket.size()) / static_cast<float>(interval);
			const float quotaF = targetPerFrame + g_lodUpdateCarry[lod];
			std::uint32_t quota = static_cast<std::uint32_t>(quotaF);
			if (quota > bucket.size())
			{
				quota = static_cast<std::uint32_t>(bucket.size());
			}
			g_lodUpdateCarry[lod] = quotaF - static_cast<float>(quota);

			const std::uint32_t n = static_cast<std::uint32_t>(bucket.size());
			const std::uint32_t start = (n > 0u) ? (g_lodUpdateCursor[lod] % n) : 0u;
			for (std::uint32_t k = 0; k < quota; ++k)
			{
				const std::uint32_t idx = bucket[(start + k) % n];
				PackedAnimatorState& packed = g_packedAnimators[idx];
				packed.animator->Update(packed.pendingDt);
				packed.pendingDt = 0.0f;
				++updatedThisFrame;
			}
			g_lodUpdateCursor[lod] = (n > 0u) ? ((start + quota) % n) : 0u;
		}

		// Compact stale entries when animators disappear from the world.
		if (!g_packedAnimators.empty())
		{
			std::vector<PackedAnimatorState> compact;
			compact.reserve(g_packedAnimators.size());
			g_animatorToPacked.clear();
			for (const PackedAnimatorState& p: g_packedAnimators)
			{
				if (!p.active || p.animator == nullptr)
				{
					continue;
				}
				const std::uint32_t idx = static_cast<std::uint32_t>(compact.size());
				g_animatorToPacked.emplace(p.animator, idx);
				compact.push_back(p);
			}
			g_packedAnimators.swap(compact);
		}

// Telemetry: plot animation system metrics to Tracy.
#ifdef TRACY_ENABLE
		{
			const std::uint32_t totalAnimators = static_cast<std::uint32_t>(g_packedAnimators.size());
			TracyPlot("Animation/TotalAnimators", static_cast<int64_t>(totalAnimators));
			TracyPlot("Animation/HeroAnimators", static_cast<int64_t>(heroCount));
			TracyPlot("Animation/UpdatedThisFrame", static_cast<int64_t>(updatedThisFrame));
			TracyPlot("Animation/LOD0_Animators", static_cast<int64_t>(animatorCountByLod[0]));
			TracyPlot("Animation/LOD1_Animators", static_cast<int64_t>(animatorCountByLod[1]));
			TracyPlot("Animation/LOD2_Animators", static_cast<int64_t>(animatorCountByLod[2]));
			TracyPlot("Animation/LOD3_Animators", static_cast<int64_t>(animatorCountByLod[3]));
		}
#endif
	}
} // namespace aether
