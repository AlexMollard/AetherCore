#include "rendering/WorldRenderer.hpp"

#include <algorithm>

#include "material/Material.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Scene.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Transform a local-space bounding sphere (xyz=center, w=radius) to world space.
		// The center is transformed by the model matrix; the radius is scaled by the
		// maximum axis scale extracted from the matrix.
		glm::vec4 TransformBoundingSphere(glm::vec4 localSphere, const glm::mat4& model)
		{
			glm::vec3 center = glm::vec3(model * glm::vec4(localSphere.x, localSphere.y, localSphere.z, 1.0f));

			// Extract max scale from the model matrix columns.
			glm::vec3 col0(model[0]);
			glm::vec3 col1(model[1]);
			glm::vec3 col2(model[2]);
			float maxScale = std::max({glm::length(col0), glm::length(col1), glm::length(col2)});

			return glm::vec4(center, localSphere.w * maxScale);
		}
	} // namespace

	void WorldRenderer::Flush(const World& world, RenderQueue& queue, const IAnimationProvider* /*anim*/)
	{
		AE_PROFILE_ZONE();
		auto view = world.GetRegistry().view<const PipelineComponent, const MeshComponent, const TransformComponent>();
		for (auto enttEntity: view)
		{
			const auto& pipelineComp = view.get<const PipelineComponent>(enttEntity);
			const auto& meshComp = view.get<const MeshComponent>(enttEntity);
			const auto& transformComp = view.get<const TransformComponent>(enttEntity);

			std::uint32_t materialIndex = Material::kNoTexture;
			if (const auto* material = world.GetRegistry().try_get<MaterialComponent>(enttEntity))
			{
				materialIndex = material->material.materialSlot;
			}

			std::int32_t skinIndex = -1;
			std::uint32_t skinJointCount = 0;
			std::uint32_t animClipIndex = 0;
			float animTime = 0.f;
			const AnimationDatabase* animDb = nullptr;
			if (const auto* smc = world.GetRegistry().try_get<SkinnedMeshComponent>(enttEntity))
			{
				if (smc->animDb && smc->animDb->IsValid())
				{
					skinIndex = static_cast<std::int32_t>(smc->skinIndex);
					skinJointCount = smc->jointCount;
					animClipIndex = std::min(smc->clipIndex, smc->animDb->GetClipCount() - 1u);
					animTime = smc->animTime;
					animDb = smc->animDb;
				}
			}

			queue.Submit({
			        .pipeline = pipelineComp.pipeline,
			        .mesh = meshComp.mesh,
			        .modelMatrix = transformComp.localToWorld,
			        .materialIndex = materialIndex,
			        .skinIndex = skinIndex,
			        .skinJointCount = skinJointCount,
			        .animClipIndex = animClipIndex,
			        .animTime = animTime,
			        .worldBoundingSphere = TransformBoundingSphere(meshComp.mesh->GetBoundingSphere(), transformComp.localToWorld),
			        .animDb = animDb,
			});
		}
	}

	void WorldRenderer::Flush(const Scene& scene, RenderQueue& queue)
	{
		AE_PROFILE_ZONE();
		for (const auto& [id, obj]: scene.m_objects)
		{
			queue.Submit({
			        .pipeline = obj.desc.pipeline,
			        .mesh = obj.desc.mesh,
			        .instanceCount = 1,
			        .modelMatrix = obj.transform,
			        .materialIndex = obj.desc.materialIndex,
			});
		}
	}
} // namespace aether
