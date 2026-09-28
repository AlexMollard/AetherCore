#include "rendering/WorldRenderer.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "material/GpuMaterial.hpp"
#include "material/MaterialRegistry.hpp"
#include "rendering/FrameConstants.hpp"

#include "physics/PhysicsComponents.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RagdollSkinDrive.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		glm::vec4 TransformBoundingSphere(glm::vec4 localSphere, const glm::mat4& model)
		{
			const glm::vec3 center = glm::vec3(model * glm::vec4(localSphere.x, localSphere.y, localSphere.z, 1.0f));

			const glm::vec3 col0(model[0]);
			const glm::vec3 col1(model[1]);
			const glm::vec3 col2(model[2]);
			const float maxScale = std::max({glm::length(col0), glm::length(col1), glm::length(col2)});

			return glm::vec4(center, localSphere.w * maxScale);
		}
	} // namespace

	void WorldRenderer::Flush(const World& world, RenderQueue& queue, glm::vec3 eyeWorldPos, bool shadowPass)
	{
		AE_PROFILE_ZONE();
		auto view = world.GetRegistry().view<const PipelineComponent, const MeshComponent, const TransformComponent>();
		for (auto enttEntity: view)
		{
			const auto& pipelineComp = view.get<const PipelineComponent>(enttEntity);
			const auto& meshComp = view.get<const MeshComponent>(enttEntity);
			const auto& transformComp = view.get<const TransformComponent>(enttEntity);

			if (!meshComp.mesh || !meshComp.mesh->IsAlive())
			{
				continue;
			}

			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttEntity)) || ecs::IsHiddenInEditor(world, World::FromEntt(enttEntity)))
			{
				continue;
			}

			if (const auto* mr = world.GetRegistry().try_get<MeshRendererComponent>(enttEntity); mr != nullptr && (!mr->visible || (shadowPass && !mr->castShadows)))
			{
				continue;
			}

			std::uint32_t materialIndex = 0xFFFFFFFFu;
			if (const auto* const material = world.GetRegistry().try_get<MaterialComponent>(enttEntity))
			{
				materialIndex = material->gpuSlot;
			}

			std::uint32_t effectParamIndex = 0xFFFFFFFFu;
			if (const auto* const fx = world.GetRegistry().try_get<EffectParamsComponent>(enttEntity))
			{
				effectParamIndex = fx->paramSlot;
			}

			std::int32_t skinIndex = -1;
			std::uint32_t skinJointCount = 0;
			std::uint32_t animClipIndex = 0;
			float animTime = 0.f;
			std::uint32_t fadeClipIndex = 0;
			float fadeTime = 0.f;
			float fadeWeight = 0.f;
			const AnimationDatabase* animDb = nullptr;
			std::span<const JointRotationOffset> jointOffsets;
			if (const auto* const smc = world.GetRegistry().try_get<SkinnedMeshComponent>(enttEntity))
			{
				if (smc->animDb && smc->animDb->IsAlive() && smc->animDb->IsValid())
				{
					skinIndex = static_cast<std::int32_t>(smc->skinIndex);
					skinJointCount = smc->jointCount;
					animClipIndex = smc->clipIndex;
					animTime = smc->animTime;
					fadeClipIndex = smc->fadeClipIndex;
					fadeTime = smc->fadeTime;
					fadeWeight = smc->fadeWeight;
					animDb = smc->animDb;
					jointOffsets = smc->jointOffsets;
				}
			}

			// Per-node override seam: RagdollSkinDriveComponent is absent and jointOffsets is
			// empty for every ordinary skinned mesh (the overwhelming common case), so this is
			// one try_get plus two empty checks - the same cost this loop already pays for
			// MaterialComponent/EffectParamsComponent above.
			std::vector<AnimationContracts::RagdollOverrideEntry> ragdollOverrides;
			if (animDb != nullptr)
			{
				if (const auto* const drive = world.GetRegistry().try_get<RagdollSkinDriveComponent>(enttEntity))
				{
					ragdollOverrides = BuildRagdollSkinOverrides(world, drive->ragdollRoot, animDb->GetNodeNames(), glm::inverse(transformComp.localToWorld));
				}
				if (!jointOffsets.empty())
				{
					AppendJointRotationOverrides(jointOffsets, ragdollOverrides);
				}
			}

			constexpr float kSkinnedMeshSphereMargin = 2.0f;
			glm::vec4 localSphere = meshComp.mesh->GetBoundingSphere();
			if (world.GetRegistry().try_get<SkinnedMeshComponent>(enttEntity) && localSphere.w > 0.0f)
			{
				localSphere.w *= kSkinnedMeshSphereMargin;
			}

			const glm::vec4 worldSphere = TransformBoundingSphere(localSphere, transformComp.localToWorld);

			std::uint32_t skyOrder = 0;
			if (pipelineComp.sky)
			{
				if (const auto* src = world.GetRegistry().try_get<MeshSourceComponent>(enttEntity))
				{
					skyOrder = src->primitiveIndex;
				}
			}

			const float viewDepthSq = glm::dot(glm::vec3(worldSphere) - eyeWorldPos, glm::vec3(worldSphere) - eyeWorldPos);
			const std::uint32_t meshGeneration = meshComp.mesh ? meshComp.mesh->GetGeneration() : 0;
			queue.Submit({
			        .pipeline = pipelineComp.pipeline,
			        .mesh = meshComp.mesh,
			        .modelMatrix = transformComp.localToWorld,
			        .materialIndex = materialIndex,
			        .effectParamIndex = effectParamIndex,
			        .skinIndex = skinIndex,
			        .skinJointCount = skinJointCount,
			        .animClipIndex = animClipIndex,
			        .animTime = animTime,
			        .fadeClipIndex = fadeClipIndex,
			        .fadeTime = fadeTime,
			        .fadeWeight = fadeWeight,
			        .worldBoundingSphere = worldSphere,
			        .ragdollOverrides = std::move(ragdollOverrides),
			        .animDb = animDb,
			        .animDbGeneration = animDb ? animDb->GetGeneration() : 0,
			        .meshGeneration = meshGeneration,
			        .blended = pipelineComp.blended,
			        .viewDepthSq = viewDepthSq,
			        .sky = pipelineComp.sky,
			        .skyOrder = skyOrder,
			});
			// Scenery foliage's soft fringe (MaterialSystem softCutout): colour only, so shadows skip it.
			// ponytail: the depth prepass draws this copy too (it flushes every draw); the depth is the
			// same, so the cost is only the extra foliage geometry there.
			if (pipelineComp.edgePipeline != nullptr && !shadowPass && skinIndex < 0)
			{
				queue.Submit({
				        .pipeline = pipelineComp.edgePipeline,
				        .mesh = meshComp.mesh,
				        .modelMatrix = transformComp.localToWorld,
				        .materialIndex = materialIndex,
				        .effectParamIndex = effectParamIndex,
				        .worldBoundingSphere = worldSphere,
				        .meshGeneration = meshGeneration,
				        .blended = true,
				        .viewDepthSq = viewDepthSq,
				});
			}
		}
	}

	std::size_t WorldRenderer::GatherBlobShadows(const World& world, const MaterialRegistry& materials, const glm::vec3 eyeWorldPos, std::vector<glm::vec4>& out)
	{
		AE_PROFILE_ZONE();
		out.clear();
		// Union of an actor's primitive bounds, as an AABB of their spheres. An actor is one
		// entity per primitive under a shared root, and one blob must stand for all of them:
		// a crate's four primitives would otherwise lay four overlapping discs.
		struct Bounds
		{
			glm::vec3 min{std::numeric_limits<float>::max()};
			glm::vec3 max{std::numeric_limits<float>::lowest()};
			bool character = false;
		};
		std::unordered_map<std::uint32_t, Bounds> actors;
		auto view = world.GetRegistry().view<const MeshComponent, const MaterialComponent, const TransformComponent>();
		for (auto enttEntity: view)
		{
			const auto& meshComp = view.get<const MeshComponent>(enttEntity);
			if (!meshComp.mesh || !meshComp.mesh->IsAlive())
			{
				continue;
			}
			const std::uint32_t flags = materials.GetFlags(view.get<const MaterialComponent>(enttEntity).handle);
			if ((flags & GpuMaterial::kObjectLit) == 0u || (flags & GpuMaterial::kAlphaBlend) != 0u)
			{
				continue;
			}
			const Entity entity = World::FromEntt(enttEntity);
			if (ecs::HasDisabledAncestor(world, entity) || ecs::IsHiddenInEditor(world, entity))
			{
				continue;
			}
			if (const auto* mr = world.GetRegistry().try_get<MeshRendererComponent>(enttEntity); mr != nullptr && !mr->visible)
			{
				continue;
			}
			const glm::vec4 sphere = TransformBoundingSphere(meshComp.mesh->GetBoundingSphere(), view.get<const TransformComponent>(enttEntity).localToWorld);
			const auto* hierarchy = world.GetRegistry().try_get<HierarchyComponent>(enttEntity);
			const std::uint32_t key = (hierarchy != nullptr && hierarchy->parent.IsValid()) ? hierarchy->parent.id : entity.id;
			Bounds& b = actors[key];
			b.min = glm::min(b.min, glm::vec3(sphere) - glm::vec3(sphere.w));
			b.max = glm::max(b.max, glm::vec3(sphere) + glm::vec3(sphere.w));
			b.character = b.character || (flags & GpuMaterial::kCharacter) != 0u;
		}

		// The blob covers the inner half of the actor's footprint and fades out past it, which
		// is the rig's size: a crate's blob just rims its base, Crash's sits under his feet.
		// Pickups (anything with a smaller blob than a crate's) get one too: the rig lays a small dark
		// blob under every wumpa at full strength although the fruit hovers about a metre up
		// (logs/wumpa/rig/idle_00.png). A negative radius tells the shader to skip the hover
		// fade for them. The floor only drops degenerate bounds.
		// ponytail: "pickup" is a size test standing in for the per-actor shadow flag the game
		// scripts carry, until that is extracted.
		constexpr float kFootprintFraction = 0.5f;
		constexpr float kMinBlobRadius = 0.05f;
		// A wumpa's bounds sphere (0.74 m, its model being 0.85 m across) gives 0.37 here, a 1 m
		// crate 0.43.
		constexpr float kPickupBlobRadius = 0.4f;
		// Character actors (a kCharacter material: Crash, Aku Aku) go first, so the shader can add
		// their contact core (CrashLook) to the first `characters` blobs, and they survive the cap.
		std::size_t characters = 0;
		for (const bool characterPass: {true, false})
		{
			for (const auto& [key, b]: actors)
			{
				if (b.character != characterPass)
				{
					continue;
				}
				const float radius = kFootprintFraction * 0.5f * std::max(b.max.x - b.min.x, b.max.z - b.min.z);
				if (radius < kMinBlobRadius)
				{
					continue;
				}
				out.emplace_back(0.5f * (b.min.x + b.max.x), b.min.y, 0.5f * (b.min.z + b.max.z), radius < kPickupBlobRadius ? -radius : radius);
				characters += characterPass ? 1u : 0u;
			}
		}
		characters = std::min<std::size_t>(characters, kMaxBlobShadows);
		const auto distSq = [&](const glm::vec4& blob) { return glm::dot(glm::vec3(blob) - eyeWorldPos, glm::vec3(blob) - eyeWorldPos); };
		if (out.size() > kMaxBlobShadows)
		{
			std::nth_element(out.begin() + characters, out.begin() + kMaxBlobShadows, out.end(), [&](const glm::vec4& a, const glm::vec4& b) { return distSq(a) < distSq(b); });
			out.resize(kMaxBlobShadows);
		}
		return characters;
	}

} // namespace aether
