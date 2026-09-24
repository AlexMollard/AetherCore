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
		void ConnectLifecycle(World& world, MaterialRegistry& registry);

		void DisconnectLifecycle(World& world);

		void AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const MaterialAsset& asset);

		void SetBaseColor(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color);
		void SetMetallic(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);
		void SetRoughness(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);
		void SetEmissive(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color);

		// Read-only counterpart of SetEmissive - the "capture the original value before
		// tinting it" half of a highlight, needed so a highlight can restore exactly what
		// was there before instead of assuming a baseline (see MaterialMeshExports.cpp's
		// own comment on the entity-material exports for why every one of these needs an
		// EntityAlive guard at the export boundary; this one additionally must not have a
		// SIDE EFFECT of seeding a MaterialInstanceComponent just from being read, unlike
		// every setter above - a getter that mutates state on a plain read would be a
		// surprising, hard-to-reason-about entry point).
		glm::vec3 GetEmissive(World& world, Entity entity, const MaterialRegistry& registry);
		void SetOcclusion(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);

		void SetAlbedoTexture(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, TextureHandle texture);
	} // namespace MaterialSystem
} // namespace aether
