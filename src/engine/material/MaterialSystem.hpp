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
		void SetOcclusion(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value);

		void SetAlbedoTexture(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, TextureHandle texture);
	} // namespace MaterialSystem
} // namespace aether
