#include "material/MaterialAuthoring.hpp"

#include <algorithm>

#include <entt/entt.hpp>

#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/World.hpp"

namespace aether
{
	std::uint32_t MaterialAuthoring::Create(const MaterialAsset& seed)
	{
		m_entries.push_back(Entry{seed, {}});
		return static_cast<std::uint32_t>(m_entries.size() - 1);
	}

	MaterialAuthoring::Entry* MaterialAuthoring::Get(std::uint32_t id)
	{
		return id < m_entries.size() ? &m_entries[id] : nullptr;
	}

	void MaterialAuthoring::Bind(World& world, Entity entity, std::uint32_t id)
	{
		Entry* entry = Get(id);
		if (entry == nullptr || !entity.IsValid())
		{
			return;
		}

		const auto prev = m_entityToId.find(entity.id);
		if (prev != m_entityToId.end() && prev->second != id)
		{
			if (Entry* prevEntry = Get(prev->second))
			{
				auto& v = prevEntry->boundEntities;
				v.erase(std::remove(v.begin(), v.end(), entity), v.end());
			}
		}
		m_entityToId[entity.id] = id;

		MaterialSystem::AssignMaterial(world, entity, m_registry, m_pipelineCache, entry->asset);

		for (const Entity bound: entry->boundEntities)
		{
			if (bound == entity)
			{
				return;
			}
		}
		entry->boundEntities.push_back(entity);
	}

	void MaterialAuthoring::Rebind(World& world, Entry& entry)
	{
		auto& r = world.GetRegistry();
		std::size_t kept = 0;
		for (std::size_t i = 0; i < entry.boundEntities.size(); ++i)
		{
			const Entity e = entry.boundEntities[i];
			if (!e.IsValid() || !r.valid(World::ToEntt(e)))
			{
				continue;
			}
			MaterialSystem::AssignMaterial(world, e, m_registry, m_pipelineCache, entry.asset);
			entry.boundEntities[kept++] = e;
		}
		entry.boundEntities.resize(kept);
	}

	void MaterialAuthoring::SetBaseColor(World& world, std::uint32_t id, const glm::vec3& color)
	{
		Entry* entry = Get(id);
		if (entry == nullptr || glm::vec3(entry->asset.baseColorFactor) == color)
		{
			return;
		}
		entry->asset.baseColorFactor = glm::vec4(color, entry->asset.baseColorFactor.w);
		Rebind(world, *entry);
	}

	void MaterialAuthoring::SetMetallic(World& world, std::uint32_t id, float value)
	{
		Entry* entry = Get(id);
		if (entry == nullptr || entry->asset.metallicFactor == value)
		{
			return;
		}
		entry->asset.metallicFactor = value;
		Rebind(world, *entry);
	}

	void MaterialAuthoring::SetRoughness(World& world, std::uint32_t id, float value)
	{
		Entry* entry = Get(id);
		if (entry == nullptr || entry->asset.roughnessFactor == value)
		{
			return;
		}
		entry->asset.roughnessFactor = value;
		Rebind(world, *entry);
	}

	void MaterialAuthoring::SetEmissive(World& world, std::uint32_t id, const glm::vec3& color)
	{
		Entry* entry = Get(id);
		if (entry == nullptr || entry->asset.emissiveFactor == color)
		{
			return;
		}
		entry->asset.emissiveFactor = color;
		Rebind(world, *entry);
	}

	void MaterialAuthoring::ReleaseAll()
	{
		m_entries.clear();
		m_entityToId.clear();
	}
} // namespace aether
