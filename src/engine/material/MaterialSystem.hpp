#pragma once

#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
	class MaterialRegistry;
	class PipelineCache;
	struct MaterialAsset;

	namespace MaterialSystem
	{
		// Connect the entt on_destroy hook so material handles are released when a
		// MaterialComponent is destroyed with its entity or removed. Call once at
		// engine setup, after the registry exists.
		void ConnectLifecycle(World& world, MaterialRegistry& registry);

		// Disconnect the hook before the registry is torn down so late world
		// teardown cannot release into a dead registry.
		void DisconnectLifecycle(World& world);

		// Assign a material to an entity: acquire the material handle (store
		// MaterialComponent{handle,slot}) and resolve its pipeline through the
		// cache (mapping authored doubleSided/alphaBlend into the template, store
		// PipelineComponent). Effect-override: if the entity has an
		// EffectParamsComponent, the pipeline is left untouched (set_entity_effect
		// owns it), so a stray set_material repaints colour without dropping the
		// effect program.
		void AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const MaterialAsset& asset);

		// -- Per-entity material instance edits ---------------------------------
		// Each seeds a MaterialInstanceComponent from defaults if the entity has
		// none, writes a single field, and re-acquires only when the value
		// actually changes (a no-op write does no pack/hash/acquire/slot churn).
		// A freshly seeded instance always assigns so the entity gains a material.
		void SetBaseColor(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color);
		void SetMetallic(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);
		void SetRoughness(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);
		void SetEmissive(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color);
		void SetOcclusion(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);

		// Set the entity's albedo (base-colour) texture map. Takes a resolved
		// TextureHandle: a broken handle (missing texture) packs to the magenta
		// fallback; an invalid handle clears the map back to flat base colour. The
		// caller owns the passed handle's ref (acquire -> set -> release); this
		// stores a copy and lets the material slot's cascade take its own ref.
		void SetAlbedoTexture(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, TextureHandle texture);
	} // namespace MaterialSystem
} // namespace aether
