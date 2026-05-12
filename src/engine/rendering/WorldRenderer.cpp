#include "rendering/WorldRenderer.hpp"

#include "animation/ModelAnimator.hpp"
#include "material/Material.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Scene.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
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

			VkDeviceAddress skinBufferAddr = 0;
			std::int32_t skinIndex = -1;
			std::uint32_t skinJointCount = 0;
			if (const auto* skin = world.GetRegistry().try_get<SkinComponent>(enttEntity))
			{
				skinBufferAddr = skin->sourceSkinBufferAddr;
				skinIndex = skin->skinIndex;
				skinJointCount = skin->jointCount;
			}

			bool nonHeroGpuBlend = false;
			std::uint32_t animClipIndex = 0;
			float animTime = 0.0f;
			bool gpuSampleEligible = false;
			if (const auto* anim = world.GetRegistry().try_get<AnimatorComponent>(enttEntity))
			{
				nonHeroGpuBlend = !anim->heroCharacter;
				if (anim->animator != nullptr && anim->animationDb != nullptr && anim->animationDb->IsValid())
				{
					animClipIndex = anim->animator->GetCurrentAnimation();
					animTime = anim->animator->GetAnimTime();
					gpuSampleEligible = true;
				}
			}

			queue.Submit({
			        .pipeline = pipelineComp.pipeline,
			        .mesh = meshComp.mesh,
			        .modelMatrix = transformComp.localToWorld,
			        .materialIndex = materialIndex,
			        .sourceSkinBufferAddr = skinBufferAddr,
			        .skinIndex = skinIndex,
			        .skinJointCount = skinJointCount,
			        .animClipIndex = animClipIndex,
			        .animTime = animTime,
			        .gpuSampleEligible = gpuSampleEligible,
			        .nonHeroGpuBlend = nonHeroGpuBlend,
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
