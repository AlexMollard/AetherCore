#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "material/MaterialAsset.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
	class MaterialRegistry;
	class PipelineCache;

	// ECS lifecycle hook, never here.
	class MaterialAuthoring
	{
	public:
		static constexpr std::uint32_t kInvalidId = 0xFFFFFFFFu;

		MaterialAuthoring(MaterialRegistry& registry, PipelineCache& pipelineCache)
		      : m_registry(registry), m_pipelineCache(pipelineCache)
		{
		}

		[[nodiscard]] std::uint32_t Create(const MaterialAsset& seed);

		void Bind(World& world, Entity entity, std::uint32_t id);

		void SetBaseColor(World& world, std::uint32_t id, const glm::vec3& color);
		void SetMetallic(World& world, std::uint32_t id, float value);
		void SetRoughness(World& world, std::uint32_t id, float value);
		void SetEmissive(World& world, std::uint32_t id, const glm::vec3& color);

		void ReleaseAll();

		[[nodiscard]] std::size_t Count() const
		{
			return m_entries.size();
		}

	private:
		struct Entry
		{
			MaterialAsset asset{};
			std::vector<Entity> boundEntities;
		};

		[[nodiscard]] Entry* Get(std::uint32_t id);
		void Rebind(World& world, Entry& entry);

		MaterialRegistry& m_registry;
		PipelineCache& m_pipelineCache;
		std::vector<Entry> m_entries;
		std::unordered_map<std::uint32_t, std::uint32_t> m_entityToId;
	};
} // namespace aether
