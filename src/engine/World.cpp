#include "World.hpp"

#include "Material.hpp"
#include "RenderQueue.hpp"

namespace aether
{
	// ── Entity lifecycle ──────────────────────────────────────────────────────

	Entity World::CreateEntity()
	{
		return Entity{ m_nextId++ };
	}

	void World::DestroyEntity(Entity entity)
	{
		const std::uint32_t id = entity.id;
		m_transforms.erase(id);
		m_meshes.erase(id);
		m_materials.erase(id);
		m_pipelines.erase(id);
		m_skins.erase(id);
	}

	// ── Component setters ─────────────────────────────────────────────────────

	void World::Set(Entity entity, TransformComponent component)
	{
		m_transforms[entity.id] = component;
	}

	void World::Set(Entity entity, MeshComponent component)
	{
		m_meshes[entity.id] = component;
	}

	void World::Set(Entity entity, MaterialComponent component)
	{
		m_materials[entity.id] = component;
	}

	void World::Set(Entity entity, PipelineComponent component)
	{
		m_pipelines[entity.id] = component;
	}

	void World::Set(Entity entity, SkinComponent component)
	{
		m_skins[entity.id] = component;
	}

	// ── Convenience spawn ────────────────────────────────────────────────────

	Entity World::SpawnMesh(GraphicsPipeline& pipeline,
	                        const Mesh&        mesh,
	                        Material           material,
	                        const glm::mat4&   transform)
	{
		const Entity entity = CreateEntity();
		Set(entity, PipelineComponent{ .pipeline = &pipeline });
		Set(entity, MeshComponent{ .mesh = &mesh });
		Set(entity, MaterialComponent{ .material = material });
		Set(entity, TransformComponent{ .localToWorld = transform });
		return entity;
	}

	// ── Component getters ─────────────────────────────────────────────────────

	TransformComponent* World::GetTransform(Entity entity)
	{
		auto it = m_transforms.find(entity.id);
		return it != m_transforms.end() ? &it->second : nullptr;
	}

	MeshComponent* World::GetMesh(Entity entity)
	{
		auto it = m_meshes.find(entity.id);
		return it != m_meshes.end() ? &it->second : nullptr;
	}

	MaterialComponent* World::GetMaterial(Entity entity)
	{
		auto it = m_materials.find(entity.id);
		return it != m_materials.end() ? &it->second : nullptr;
	}

	PipelineComponent* World::GetPipeline(Entity entity)
	{
		auto it = m_pipelines.find(entity.id);
		return it != m_pipelines.end() ? &it->second : nullptr;
	}

	SkinComponent* World::GetSkin(Entity entity)
	{
		auto it = m_skins.find(entity.id);
		return it != m_skins.end() ? &it->second : nullptr;
	}

	const TransformComponent* World::GetTransform(Entity entity) const
	{
		auto it = m_transforms.find(entity.id);
		return it != m_transforms.end() ? &it->second : nullptr;
	}

	const MeshComponent* World::GetMesh(Entity entity) const
	{
		auto it = m_meshes.find(entity.id);
		return it != m_meshes.end() ? &it->second : nullptr;
	}

	const MaterialComponent* World::GetMaterial(Entity entity) const
	{
		auto it = m_materials.find(entity.id);
		return it != m_materials.end() ? &it->second : nullptr;
	}

	const PipelineComponent* World::GetPipeline(Entity entity) const
	{
		auto it = m_pipelines.find(entity.id);
		return it != m_pipelines.end() ? &it->second : nullptr;
	}

	const SkinComponent* World::GetSkin(Entity entity) const
	{
		auto it = m_skins.find(entity.id);
		return it != m_skins.end() ? &it->second : nullptr;
	}

	// ── Flush ─────────────────────────────────────────────────────────────────

	void World::FlushToQueue(RenderQueue& queue) const
	{
		for (const auto& [id, pipelineComp] : m_pipelines)
		{
			const auto meshIt      = m_meshes.find(id);
			const auto transformIt = m_transforms.find(id);

			if (meshIt == m_meshes.end() || transformIt == m_transforms.end())
			{
				continue;
			}

			std::uint32_t materialIndex = Material::kNoTexture;
			if (const auto matIt = m_materials.find(id); matIt != m_materials.end())
			{
				materialIndex = matIt->second.material.materialSlot;
			}

			VkDeviceAddress skinBufferAddr = 0;
			if (const auto skinIt = m_skins.find(id); skinIt != m_skins.end())
			{
				skinBufferAddr = skinIt->second.skinBufferAddr;
			}

			queue.Submit({
				.pipeline      = pipelineComp.pipeline,
				.mesh          = meshIt->second.mesh,
				.modelMatrix   = transformIt->second.localToWorld,
				.materialIndex = materialIndex,
				.skinBufferAddr = skinBufferAddr,
			});
		}
	}
}
