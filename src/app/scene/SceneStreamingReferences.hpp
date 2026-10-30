#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

namespace aether::app::scene
{
	struct StreamingFieldReference
	{
		std::uint64_t owner;
		std::string component;
		std::string field;
		reflect::FieldValue stable;
		reflect::FieldValue applied;
	};

	struct StreamingFieldReferenceState
	{
		std::vector<StreamingFieldReference> entries;
	};

	inline bool SameStreamingFieldValue(const reflect::FieldValue& a, const reflect::FieldValue& b)
	{
		if (a.type != b.type || a.num != b.num || a.boolean != b.boolean || a.enumValue != b.enumValue || a.entity != b.entity || a.str != b.str || a.vec != b.vec || a.list.size() != b.list.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < a.list.size(); ++i)
		{
			if (a.list[i].size() != b.list[i].size())
			{
				return false;
			}
			for (std::size_t j = 0; j < a.list[i].size(); ++j)
			{
				if (!SameStreamingFieldValue(a.list[i][j], b.list[i][j]))
				{
					return false;
				}
			}
		}
		return true;
	}

	struct StreamingNodeIndex
	{
		std::unordered_map<std::uint64_t, Entity> exact;
		std::unordered_map<std::uint64_t, Entity> aliases;
		std::unordered_set<std::uint64_t> ambiguousExact, ambiguousAliases;
		std::unordered_map<std::uint64_t, std::uint64_t> byEntity;

		explicit StreamingNodeIndex(World& world)
		{
			world.View<SceneNodeComponent>().each(
			        [&](entt::entity handle, const SceneNodeComponent& node)
			        {
				        const auto entity = World::FromEntt(handle);
				        if (node.id)
				        {
					        if (!exact.emplace(node.id, entity).second)
					        {
						        ambiguousExact.insert(node.id);
					        }
					        byEntity.emplace(entity.id, node.id);
				        }
				        if (node.sourceId && !aliases.emplace(node.sourceId, entity).second)
				        {
					        ambiguousAliases.insert(node.sourceId);
				        }
			        });
		}

		Expected<Entity> Resolve(std::uint64_t id) const
		{
			if (!id)
			{
				return Entity{};
			}
			if (ambiguousExact.contains(id))
			{
				return std::unexpected(AetherError::Engine("Ambiguous scene node identity"));
			}
			if (const auto it = exact.find(id); it != exact.end())
			{
				return it->second;
			}
			if (ambiguousAliases.contains(id))
			{
				return std::unexpected(AetherError::Engine("Ambiguous source scene node reference"));
			}
			if (const auto it = aliases.find(id); it != aliases.end())
			{
				return it->second;
			}
			return Entity{};
		}
	};

	inline bool StreamingMapField(reflect::FieldValue& value, const std::unordered_map<std::uint64_t, std::uint64_t>& mapping, bool clear, bool& found)
	{
		if (value.type == reflect::FieldType::EntityRef)
		{
			found = true;
			if (value.entity)
			{
				if (clear)
				{
					value.entity = 0;
				}
				else if (const auto it = mapping.find(value.entity); it != mapping.end())
				{
					value.entity = it->second;
				}
				else
				{
					return false;
				}
			}
		}
		for (auto& row: value.list)
		{
			for (auto& child: row)
			{
				if (!StreamingMapField(child, mapping, clear, found))
				{
					return false;
				}
			}
		}
		return true;
	}

	inline Expected<void> ResolveStreamingFieldValue(reflect::FieldValue& stable, reflect::FieldValue& applied, const StreamingNodeIndex& index, bool& missing)
	{
		if (stable.type == reflect::FieldType::EntityRef && stable.entity)
		{
			auto target = index.Resolve(stable.entity);
			if (!target)
			{
				return std::unexpected(target.error());
			}
			if (target->IsValid())
			{
				stable.entity = index.byEntity.at(target->id);
				applied.entity = target->id;
			}
			else
			{
				applied.entity = 0;
				missing = true;
			}
		}
		for (std::size_t i = 0; i < stable.list.size(); ++i)
		{
			for (std::size_t j = 0; j < stable.list[i].size(); ++j)
			{
				if (auto result = ResolveStreamingFieldValue(stable.list[i][j], applied.list[i][j], index, missing); !result)
				{
					return result;
				}
			}
		}
		return {};
	}

	inline StreamingFieldReferenceState& StreamingFieldReferences(World& world)
	{
		auto& ctx = world.GetRegistry().ctx();
		return ctx.contains<StreamingFieldReferenceState>() ? ctx.get<StreamingFieldReferenceState>() : ctx.emplace<StreamingFieldReferenceState>();
	}

	inline Expected<void> ApplyStreamingFieldReference(World& world, Entity owner, const reflect::ComponentType& type, const reflect::FieldDesc& field, void* component, const reflect::FieldValue& saved, const StreamingNodeIndex& index)
	{
		const auto* node = world.TryGet<SceneNodeComponent>(owner);
		if (!node || !node->id || !field.set)
		{
			return std::unexpected(AetherError::Engine("Streaming reflected field owner missing"));
		}
		auto stable = saved, applied = saved;
		bool missing = false;
		if (auto result = ResolveStreamingFieldValue(stable, applied, index, missing); !result)
		{
			return result;
		}
		auto& entries = StreamingFieldReferences(world).entries;
		std::erase_if(entries, [&](const auto& p) { return p.owner == node->id && p.component == type.name && p.field == field.name; });
		field.set(component, applied);
		if (missing)
		{
			entries.push_back({node->id, type.name, field.name, std::move(stable), std::move(applied)});
		}
		return {};
	}

	inline Expected<void> CaptureStreamingFieldReference(World& world, Entity owner, std::string_view component, std::string_view field, reflect::FieldValue& value, const StreamingNodeIndex& index)
	{
		const auto* node = world.TryGet<SceneNodeComponent>(owner);
		if (!node)
		{
			return std::unexpected(AetherError::Engine("Streaming reflected owner has no node"));
		}
		auto& entries = StreamingFieldReferences(world).entries;
		const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& p) { return p.owner == node->id && p.component == component && p.field == field && SameStreamingFieldValue(p.applied, value); });
		if (it != entries.end())
		{
			value = it->stable;
		}
		else
		{
			bool found = false;
			if (!StreamingMapField(value, index.byEntity, false, found))
			{
				return std::unexpected(AetherError::Engine("Streaming reflected target has no live stable node"));
			}
		}
		return {};
	}

	inline bool StreamingFieldReferencesAny(const reflect::FieldValue& value, const std::unordered_set<std::uint64_t>& entities)
	{
		if (value.type == reflect::FieldType::EntityRef && entities.contains(value.entity))
		{
			return true;
		}
		for (const auto& row: value.list)
		{
			for (const auto& child: row)
			{
				if (StreamingFieldReferencesAny(child, entities))
				{
					return true;
				}
			}
		}
		return false;
	}

	inline Expected<void> PrepareStreamingFieldReferencesForDetach(World& world, const std::vector<Entity>& entities)
	{
		StreamingNodeIndex index(world);
		std::unordered_set<std::uint64_t> detached;
		for (const auto entity: entities)
		{
			const auto it = index.byEntity.find(entity.id);
			if (it == index.byEntity.end())
			{
				return std::unexpected(AetherError::Engine("Detached streaming entity has no stable node"));
			}
			detached.insert(entity.id);
			index.exact.erase(it->second);
		}
		for (auto it = index.aliases.begin(); it != index.aliases.end();)
		{
			if (detached.contains(it->second.id))
			{
				it = index.aliases.erase(it);
			}
			else
			{
				++it;
			}
		}

		struct Update
		{
			Entity owner;
			const reflect::ComponentType* type;
			const reflect::FieldDesc* field;
			void* component;
			reflect::FieldValue stable;
		};

		std::vector<Update> updates;
		Expected<void> result{};
		world.View<SceneNodeComponent>().each(
		        [&](entt::entity handle, const SceneNodeComponent& node)
		        {
			        const auto owner = World::FromEntt(handle);
			        if (!result || !node.id || detached.contains(owner.id))
			        {
				        return;
			        }
			        for (const auto& type: reflect::ComponentTypes())
			        {
				        if (!type.genericSerialize || !type.tryGetRaw)
				        {
					        continue;
				        }
				        auto* component = type.tryGetRaw(world, owner);
				        if (!component)
				        {
					        continue;
				        }
				        for (const auto& field: type.fields)
				        {
					        if (!field.get || !field.set || (field.type != reflect::FieldType::EntityRef && field.type != reflect::FieldType::List))
					        {
						        continue;
					        }
					        auto stable = field.get(component);
					        if (!StreamingFieldReferencesAny(stable, detached))
					        {
						        continue;
					        }
					        result = CaptureStreamingFieldReference(world, owner, type.name, field.name, stable, index);
					        if (!result)
					        {
						        return;
					        }
					        updates.push_back({owner, &type, &field, component, std::move(stable)});
				        }
			        }
		        });
		if (!result)
		{
			return result;
		}
		for (auto& update: updates)
		{
			if (auto applied = ApplyStreamingFieldReference(world, update.owner, *update.type, *update.field, update.component, update.stable, index); !applied)
			{
				return applied;
			}
		}
		return {};
	}

	inline Expected<void> ResolveStreamingFieldReferences(World& world)
	{
		auto& entries = StreamingFieldReferences(world).entries;
		if (entries.empty())
		{
			return {};
		}
		StreamingNodeIndex index(world);
		for (auto it = entries.begin(); it != entries.end();)
		{
			const auto owner = index.exact.find(it->owner);
			if (owner == index.exact.end())
			{
				++it;
				continue;
			}
			const auto* type = reflect::FindComponentType(it->component);
			const auto* field = type ? type->FindField(it->field) : nullptr;
			auto* component = type && type->tryGetRaw ? type->tryGetRaw(world, owner->second) : nullptr;
			if (!component || !field || !field->get || !field->set || !SameStreamingFieldValue(field->get(component), it->applied))
			{
				it = entries.erase(it);
				continue;
			}
			auto applied = it->stable;
			bool missing = false;
			if (auto result = ResolveStreamingFieldValue(it->stable, applied, index, missing); !result)
			{
				return result;
			}
			if (!SameStreamingFieldValue(applied, it->applied))
			{
				field->set(component, applied);
				it->applied = std::move(applied);
			}
			if (!missing)
			{
				it = entries.erase(it);
			}
			else
			{
				++it;
			}
		}
		return {};
	}

	inline void ForgetStreamingFieldReferences(World& world, const std::vector<Entity>& entities)
	{
		std::unordered_set<std::uint64_t> nodes;
		for (const auto entity: entities)
		{
			if (const auto* node = world.TryGet<SceneNodeComponent>(entity))
			{
				nodes.insert(node->id);
			}
		}
		auto& entries = StreamingFieldReferences(world).entries;
		std::erase_if(entries, [&](const auto& p) { return nodes.contains(p.owner); });
	}
} // namespace aether::app::scene
