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

	// Named, shareable authoring materials layered over the immutable
	// MaterialRegistry. A material is created once (returns an opaque uint32 id),
	// edited field-by-field, and bound to any number of entities; editing an id
	// re-acquires and re-binds every entity currently bound to it.
	//
	// Owns no GPU state and allocates no slots: Create/Bind/edits all route
	// through MaterialSystem::AssignMaterial, so content dedup, the full-buffer
	// fallback, and deferred slot recycling are inherited from phase 1. Entity
	// material handles live on their MaterialComponents and are released by the
	// ECS lifecycle hook, never here.
	//
	// Thread safety: game-thread only (all callers are das bindings, which run
	// under the thread_local script context). No internal locking.
	class MaterialAuthoring
	{
	public:
		static constexpr std::uint32_t kInvalidId = 0xFFFFFFFFu;

		MaterialAuthoring(MaterialRegistry& registry, PipelineCache& pipelineCache)
		      : m_registry(registry), m_pipelineCache(pipelineCache)
		{
		}

		// Create an authored material from a seed asset; returns its id.
		[[nodiscard]] std::uint32_t Create(const MaterialAsset& seed);

		// Bind an authored material to an entity (assigns + tracks for re-bind).
		void Bind(World& world, Entity entity, std::uint32_t id);

		// Field edits. Each mutates the authored asset (no-op if unchanged) and
		// re-binds every live entity currently bound to the id.
		void SetBaseColor(World& world, std::uint32_t id, const glm::vec3& color);
		void SetMetallic(World& world, std::uint32_t id, float value);
		void SetRoughness(World& world, std::uint32_t id, float value);
		void SetEmissive(World& world, std::uint32_t id, const glm::vec3& color);

		// Drop all authoring bookkeeping (call on scene teardown/shutdown). Pure
		// CPU: no registry/sink interaction, so it is always safe to call even
		// after the material buffer has shut down.
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
		// Entity -> the id it is currently bound to, so re-binding an entity to a
		// different material untracks it from the previous one (otherwise a later
		// edit of the old material would clobber the entity's current material).
		std::unordered_map<std::uint32_t, std::uint32_t> m_entityToId;
	};
} // namespace aether
