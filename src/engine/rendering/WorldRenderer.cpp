#include "rendering/WorldRenderer.hpp"

#include <algorithm>

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

			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttEntity)))
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
			const AnimationDatabase* animDb = nullptr;
			if (const auto* const smc = world.GetRegistry().try_get<SkinnedMeshComponent>(enttEntity))
			{
				if (smc->animDb && smc->animDb->IsAlive() && smc->animDb->IsValid())
				{
					skinIndex = static_cast<std::int32_t>(smc->skinIndex);
					skinJointCount = smc->jointCount;
					animClipIndex = smc->clipIndex;
					animTime = smc->animTime;
					animDb = smc->animDb;
				}
			}

			constexpr float kSkinnedMeshSphereMargin = 2.0f;
			glm::vec4 localSphere = meshComp.mesh->GetBoundingSphere();
			if (world.GetRegistry().try_get<SkinnedMeshComponent>(enttEntity) && localSphere.w > 0.0f)
			{
				localSphere.w *= kSkinnedMeshSphereMargin;
			}

			const glm::vec4 worldSphere = TransformBoundingSphere(localSphere, transformComp.localToWorld);

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
			        .worldBoundingSphere = worldSphere,
			        .animDb = animDb,
			        .animDbGeneration = animDb ? animDb->GetGeneration() : 0,
			        .meshGeneration = meshComp.mesh ? meshComp.mesh->GetGeneration() : 0,
			        .blended = pipelineComp.blended,
			        .viewDepthSq = glm::dot(glm::vec3(worldSphere) - eyeWorldPos, glm::vec3(worldSphere) - eyeWorldPos),
			});
		}
	}

} // namespace aether
