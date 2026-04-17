#include "World.hpp"

#include "Material.hpp"
#include "ModelAnimator.hpp"
#include "Profiler.hpp"
#include "RenderQueue.hpp"

namespace aether
{
	entt::entity World::ToEntt(Entity entity) noexcept
	{
		return static_cast<entt::entity>(entity.id);
	}

	Entity World::FromEntt(entt::entity entity) noexcept
	{
		return Entity{ static_cast<std::uint32_t>(entt::to_integral(entity)) };
	}

	// ── Entity lifecycle ──────────────────────────────────────────────────────

	Entity World::Create()
	{
		return FromEntt(m_registry.create());
	}

	void World::Destroy(Entity entity)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (entity.IsValid() && m_registry.valid(enttEntity))
		{
			m_registry.destroy(enttEntity);
		}
	}

	void World::FlushToQueue(RenderQueue& queue) const
	{
		AE_PROFILE_ZONE();
		auto view = m_registry.view<const PipelineComponent, const MeshComponent, const TransformComponent>();
		for (auto enttEntity: view)
		{
			const auto& pipelineComp = view.get<const PipelineComponent>(enttEntity);
			const auto& meshComp = view.get<const MeshComponent>(enttEntity);
			const auto& transformComp = view.get<const TransformComponent>(enttEntity);

			std::uint32_t materialIndex = Material::kNoTexture;
			if (const auto* material = m_registry.try_get<MaterialComponent>(enttEntity))
			{
				materialIndex = material->material.materialSlot;
			}

			VkDeviceAddress skinBufferAddr = 0;
			std::int32_t skinIndex = -1;
			std::uint32_t skinJointCount = 0;
			if (const auto* skin = m_registry.try_get<SkinComponent>(enttEntity))
			{
				skinBufferAddr = skin->sourceSkinBufferAddr;
				skinIndex = skin->skinIndex;
				skinJointCount = skin->jointCount;
			}

			bool nonHeroGpuBlend = false;
			std::uint32_t animClipIndex = 0;
			float animTime = 0.0f;
			bool gpuSampleEligible = false;
			if (const auto* anim = m_registry.try_get<AnimatorComponent>(enttEntity))
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

	void World::RegisterSystem(std::unique_ptr<System> system)
	{
		if (system)
		{
			system->OnRegister(*this);
			m_systems.Register(std::move(system));
		}
	}

	void World::UnregisterSystem(const char* name)
	{
		m_systems.Unregister(name);
	}

	void World::UpdateSystems(float dt)
	{
		AE_PROFILE_ZONE();
		m_systems.UpdateAll(*this, dt);
	}
} // namespace aether
