#pragma once

#include <cstdint>
#include <unordered_map>

#include "Components.hpp"
#include "Entity.hpp"

namespace aether
{
	class RenderQueue;

	// Lightweight ECS world.  Components are stored in per-type hash maps keyed
	// by entity id.  All methods are O(1) on average.
	//
	// Entities must have at minimum a PipelineComponent + MeshComponent +
	// TransformComponent to be emitted by FlushToQueue().  MaterialComponent is
	// optional — entities without it fall back to vertex colour in the shader.
	class World
	{
	public:
		// ── Entity lifecycle ──────────────────────────────────────────────────

		[[nodiscard]] Entity CreateEntity();
		void DestroyEntity(Entity entity);

		// ── Component setters ─────────────────────────────────────────────────

		void Set(Entity entity, TransformComponent component);
		void Set(Entity entity, MeshComponent     component);
		void Set(Entity entity, MaterialComponent  component);
		void Set(Entity entity, PipelineComponent  component);
		void Set(Entity entity, SkinComponent      component);

		// ── Component getters (return nullptr when component is absent) ───────

		[[nodiscard]] TransformComponent* GetTransform(Entity entity);
		[[nodiscard]] MeshComponent*      GetMesh     (Entity entity);
		[[nodiscard]] MaterialComponent*  GetMaterial (Entity entity);
		[[nodiscard]] PipelineComponent*  GetPipeline (Entity entity);
		[[nodiscard]] SkinComponent*      GetSkin     (Entity entity);

		[[nodiscard]] const TransformComponent* GetTransform(Entity entity) const;
		[[nodiscard]] const MeshComponent*      GetMesh     (Entity entity) const;
		[[nodiscard]] const MaterialComponent*  GetMaterial (Entity entity) const;
		[[nodiscard]] const PipelineComponent*  GetPipeline (Entity entity) const;
		[[nodiscard]] const SkinComponent*      GetSkin     (Entity entity) const;

		// ── Engine-internal ───────────────────────────────────────────────────

		// Emits a DrawCommand for every entity that has Pipeline + Mesh + Transform.
		// MaterialComponent is used if present, otherwise albedoSlot = kNoTexture.
		void FlushToQueue(RenderQueue& queue) const;

	private:
		std::uint32_t m_nextId = 1;

		std::unordered_map<std::uint32_t, TransformComponent> m_transforms;
		std::unordered_map<std::uint32_t, MeshComponent>      m_meshes;
		std::unordered_map<std::uint32_t, MaterialComponent>  m_materials;
		std::unordered_map<std::uint32_t, PipelineComponent>  m_pipelines;
		std::unordered_map<std::uint32_t, SkinComponent>      m_skins;
	};
}
