#include "rendering/WorldRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "material/GpuMaterial.hpp"
#include "material/MaterialRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/SceneEffectExtraction.hpp"

#include "physics/PhysicsComponents.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RagdollSkinDrive.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		glm::vec4 TransformBoundingSphere(glm::vec4 localSphere, const glm::mat4& model)
		{
			return TransformEffectBoundingSphere(localSphere, model);
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
		std::vector<BlobShadowSource> sources;
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
			sources.push_back({key, sphere, (flags & GpuMaterial::kCharacter) != 0u});
		}
		return ExtractBlobShadows(sources, eyeWorldPos, kMaxBlobShadows, out);
	}

	void WorldRenderer::GatherDarknessVolumes(const World& world, const glm::vec3 eyeWorldPos, std::vector<glm::vec4>& out)
	{
		AE_PROFILE_ZONE();
		out.clear();
		std::vector<DarknessVolumeSource> sources;
		auto view = world.GetRegistry().view<const DarknessVolumeComponent, const TransformComponent>();
		for (auto enttEntity: view)
		{
			const Entity entity = World::FromEntt(enttEntity);
			if (ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			sources.push_back({view.get<const TransformComponent>(enttEntity).localToWorld, view.get<const DarknessVolumeComponent>(enttEntity).fadeDepth});
		}
		ExtractDarknessVolumes(sources, eyeWorldPos, kMaxDarknessVolumes, out);
	}

} // namespace aether
