#include "debug/EditorCommand.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/AssetManager.hpp"
#include "assets/TileAssetStore.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	TransformCommand::TransformCommand(std::vector<Item> items)
	      : m_items(std::move(items))
	{
	}

	void TransformCommand::Undo(World& world, ServiceContainer&)
	{
		for (const Item& item: m_items)
		{
			const Entity entity{item.id};
			if (entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)))
			{
				ecs::SetWorldTransform(world, entity, item.before);
			}
		}
	}

	void TransformCommand::Redo(World& world, ServiceContainer&)
	{
		for (const Item& item: m_items)
		{
			const Entity entity{item.id};
			if (entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)))
			{
				ecs::SetWorldTransform(world, entity, item.after);
			}
		}
	}

	SpriteAnimationEditCommand::SpriteAnimationEditCommand(SpriteAnimationAsset before, SpriteAnimationAsset after, std::function<void(const SpriteAnimationAsset&)> apply)
	      : m_before(std::move(before))
	      , m_after(std::move(after))
	      , m_apply(std::move(apply))
	{
	}

	void SpriteAnimationEditCommand::Undo(World&, ServiceContainer&)
	{
		if (m_apply)
		{
			m_apply(m_before);
		}
	}

	void SpriteAnimationEditCommand::Redo(World&, ServiceContainer&)
	{
		if (m_apply)
		{
			m_apply(m_after);
		}
	}

	TileStrokeCommand::TileStrokeCommand(std::string tilemapPath, std::vector<TilePaintEdit> edits)
	      : m_tilemapPath(std::move(tilemapPath))
	      , m_edits(std::move(edits))
	{
	}

	void TileStrokeCommand::Apply(ServiceContainer& services, bool forward)
	{
		auto* tiles = services.TryGet<TileAssetStore>();
		if (tiles == nullptr)
		{
			return;
		}
		TileMapAsset* map = tiles->MutableTileMap(m_tilemapPath);
		if (map == nullptr)
		{
			return;
		}
		for (const TilePaintEdit& edit: m_edits)
		{
			map->SetCell(edit.layer, edit.cell, forward ? edit.after : edit.before);
		}
	}

	void TileStrokeCommand::Undo(World&, ServiceContainer& services)
	{
		Apply(services, /*forward=*/false);
	}

	void TileStrokeCommand::Redo(World&, ServiceContainer& services)
	{
		Apply(services, /*forward=*/true);
	}

	std::unique_ptr<SubtreeLifetimeCommand> SubtreeLifetimeCommand::Capture(World& world, ServiceContainer& services, const std::vector<Entity>& roots, bool createdByThisEdit, const char* label)
	{
		auto* assets = services.TryGet<AssetManager>();
		if (assets == nullptr || roots.empty())
		{
			return nullptr;
		}
		app::scene::SceneDescription subtree = app::scene::CaptureSubtrees(world, roots, assets->GetMaterialRegistry(), assets->GetTextureRegistry());
		if (subtree.entities.empty())
		{
			return nullptr;
		}
		std::vector<std::uint32_t> parentIds(subtree.entities.size(), 0);
		for (std::size_t i = 0; i < subtree.entities.size(); ++i)
		{
			if (subtree.entities[i].parentIndex >= 0)
			{
				continue;
			}
			const Entity entity{subtree.entities[i].entityId};
			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			{
				parentIds[i] = hierarchy->parent.id;
			}
		}
		return std::make_unique<SubtreeLifetimeCommand>(std::move(subtree), std::move(parentIds), createdByThisEdit, label);
	}

	SubtreeLifetimeCommand::SubtreeLifetimeCommand(app::scene::SceneDescription subtree, std::vector<std::uint32_t> parentIds, bool createdByThisEdit, const char* label)
	      : m_subtree(std::move(subtree))
	      , m_parentIds(std::move(parentIds))
	      , m_createdByThisEdit(createdByThisEdit)
	      , m_label(label)
	{
	}

	void SubtreeLifetimeCommand::Recreate(World& world, ServiceContainer& services)
	{
		// Restore each subtree onto its original ids (roots left detached), then
		// reattach every root to the parent it had when captured.
		m_lastRecreated = app::scene::RestoreSubtreeInPlace(m_subtree, world, app::scene::MakeApplySceneDeps(services), Entity{});
		for (std::size_t i = 0; i < m_subtree.entities.size() && i < m_lastRecreated.size(); ++i)
		{
			if (m_subtree.entities[i].parentIndex >= 0)
			{
				continue;
			}
			const Entity parent{i < m_parentIds.size() ? m_parentIds[i] : 0};
			if (parent.IsValid() && world.GetRegistry().valid(World::ToEntt(parent)) && world.GetRegistry().valid(World::ToEntt(m_lastRecreated[i])))
			{
				ecs::SetParent(world, m_lastRecreated[i], parent);
			}
		}
	}

	void SubtreeLifetimeCommand::DestroyAll(World& world)
	{
		m_lastRecreated.clear();
		for (const app::scene::EntityRecord& record: m_subtree.entities)
		{
			const Entity entity{record.entityId};
			if (entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)))
			{
				ecs::DetachFromParent(world, entity);
				world.Destroy(entity);
			}
		}
	}

	void SubtreeLifetimeCommand::Undo(World& world, ServiceContainer& services)
	{
		if (m_createdByThisEdit)
		{
			DestroyAll(world);
		}
		else
		{
			Recreate(world, services);
		}
	}

	void SubtreeLifetimeCommand::Redo(World& world, ServiceContainer& services)
	{
		if (m_createdByThisEdit)
		{
			Recreate(world, services);
		}
		else
		{
			DestroyAll(world);
		}
	}

	Entity SubtreeLifetimeCommand::Remap(Entity entity) const
	{
		if (!entity.IsValid())
		{
			return entity;
		}
		for (std::size_t i = 0; i < m_subtree.entities.size() && i < m_lastRecreated.size(); ++i)
		{
			if (m_subtree.entities[i].entityId == entity.id)
			{
				return m_lastRecreated[i];
			}
		}
		return entity;
	}

	namespace
	{
		using app::scene::EntityRecord;
		using app::scene::SceneDescription;

		std::unordered_map<std::uint32_t, std::size_t> BuildIndex(const SceneDescription& desc)
		{
			std::unordered_map<std::uint32_t, std::size_t> index;
			index.reserve(desc.entities.size());
			for (std::size_t i = 0; i < desc.entities.size(); ++i)
			{
				index[desc.entities[i].entityId] = i;
			}
			return index;
		}

		std::vector<std::vector<std::size_t>> BuildChildren(const SceneDescription& desc)
		{
			std::vector<std::vector<std::size_t>> children(desc.entities.size());
			for (std::size_t i = 0; i < desc.entities.size(); ++i)
			{
				const int parent = desc.entities[i].parentIndex;
				if (parent >= 0 && static_cast<std::size_t>(parent) < desc.entities.size())
				{
					children[static_cast<std::size_t>(parent)].push_back(i);
				}
			}
			return children;
		}

		std::size_t RootIndexOf(const SceneDescription& desc, std::size_t start)
		{
			std::size_t cur = start;
			for (std::size_t guard = 0; guard < desc.entities.size() + 1; ++guard)
			{
				const int parent = desc.entities[cur].parentIndex;
				if (parent < 0 || static_cast<std::size_t>(parent) >= desc.entities.size())
				{
					break;
				}
				cur = static_cast<std::size_t>(parent);
			}
			return cur;
		}

		std::uint32_t ParentIdOf(const SceneDescription& desc, std::size_t i)
		{
			const int parent = desc.entities[i].parentIndex;
			return (parent >= 0 && static_cast<std::size_t>(parent) < desc.entities.size()) ? desc.entities[static_cast<std::size_t>(parent)].entityId : 0u;
		}

		// Content identity of a record: its own serialized fields plus its parent's
		// id. Independent of array position, so an index shift alone is not a change,
		// but a component edit or a reparent is.
		std::string RecordKey(const SceneDescription& desc, std::size_t i)
		{
			SceneDescription mini;
			mini.entities.push_back(desc.entities[i]);
			EntityRecord& record = mini.entities[0];
			record.parentIndex = -1;
			// A SpriteAnimator drives the renderer's frame (spriteId/uvRect/pixel
			// size/pivot) every frame during edit-mode preview. That is runtime state,
			// not an authored edit, so canonicalize it here - otherwise a previewing
			// animation would register as an edit and flood undo with frame changes.
			if (record.spriteAnimator.has_value() && record.sprite.has_value())
			{
				record.sprite->spriteId = {};
				record.sprite->uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
				record.sprite->pixelSize = glm::vec2(0.0f);
				record.sprite->pivot = glm::vec2(0.0f);
			}
			return app::scene::WriteToml(mini) + "|p=" + std::to_string(ParentIdOf(desc, i));
		}

		// Extract the top-level subtree rooted at rootIndex (root + all descendants)
		// as a self-contained snapshot with subtree-local parentIndex. Records are
		// ordered parent-before-child.
		void ExtractSubtree(const SceneDescription& full, std::size_t rootIndex, const std::vector<std::vector<std::size_t>>& children, SceneDescription& outDesc, std::uint32_t& outParentId, std::vector<std::uint32_t>& outIds)
		{
			std::vector<std::size_t> order;
			std::vector<std::size_t> stack{rootIndex};
			while (!stack.empty())
			{
				const std::size_t node = stack.back();
				stack.pop_back();
				order.push_back(node);
				for (const std::size_t child: children[node])
				{
					stack.push_back(child);
				}
			}
			std::unordered_map<std::size_t, std::size_t> localOf;
			localOf.reserve(order.size());
			for (std::size_t k = 0; k < order.size(); ++k)
			{
				localOf[order[k]] = k;
			}
			outDesc = SceneDescription{};
			outDesc.kind = full.kind;
			outDesc.features = full.features;
			outIds.clear();
			outIds.reserve(order.size());
			for (const std::size_t fullIndex: order)
			{
				EntityRecord record = full.entities[fullIndex];
				const int parent = record.parentIndex;
				if (parent >= 0 && localOf.contains(static_cast<std::size_t>(parent)))
				{
					record.parentIndex = static_cast<int>(localOf[static_cast<std::size_t>(parent)]);
				}
				else
				{
					record.parentIndex = -1;
				}
				outDesc.entities.push_back(std::move(record));
				outIds.push_back(full.entities[fullIndex].entityId);
			}
			outParentId = ParentIdOf(full, rootIndex);
		}
	} // namespace

	EntityDiffCommand::EntityDiffCommand(const app::scene::SceneDescription& before, const app::scene::SceneDescription& after)
	{
		const std::unordered_map<std::uint32_t, std::size_t> beforeIndex = BuildIndex(before);
		const std::unordered_map<std::uint32_t, std::size_t> afterIndex = BuildIndex(after);

		// Which entities changed: created, deleted, or content-edited.
		std::unordered_set<std::uint32_t> changed;
		for (const auto& [id, bi]: beforeIndex)
		{
			const auto it = afterIndex.find(id);
			if (it == afterIndex.end() || RecordKey(before, bi) != RecordKey(after, it->second))
			{
				changed.insert(id);
			}
		}
		for (const auto& [id, ai]: afterIndex)
		{
			if (!beforeIndex.contains(id))
			{
				changed.insert(id);
			}
		}
		if (changed.empty())
		{
			return;
		}

		// The top-level subtree root each change belongs to, in both states.
		std::unordered_set<std::uint32_t> rootIds;
		for (const std::uint32_t id: changed)
		{
			if (const auto it = beforeIndex.find(id); it != beforeIndex.end())
			{
				rootIds.insert(before.entities[RootIndexOf(before, it->second)].entityId);
			}
			if (const auto it = afterIndex.find(id); it != afterIndex.end())
			{
				rootIds.insert(after.entities[RootIndexOf(after, it->second)].entityId);
			}
		}

		const std::vector<std::vector<std::size_t>> beforeChildren = BuildChildren(before);
		const std::vector<std::vector<std::size_t>> afterChildren = BuildChildren(after);
		for (const std::uint32_t rootId: rootIds)
		{
			Group group;
			if (const auto it = beforeIndex.find(rootId); it != beforeIndex.end())
			{
				group.existsBefore = true;
				ExtractSubtree(before, it->second, beforeChildren, group.beforeSubtree, group.beforeParent, group.beforeIds);
			}
			if (const auto it = afterIndex.find(rootId); it != afterIndex.end())
			{
				group.existsAfter = true;
				ExtractSubtree(after, it->second, afterChildren, group.afterSubtree, group.afterParent, group.afterIds);
			}
			m_groups.push_back(std::move(group));
		}
	}

	void EntityDiffCommand::ApplyState(Group& group, bool toAfter, World& world, ServiceContainer& services)
	{
		const bool targetExists = toAfter ? group.existsAfter : group.existsBefore;
		const app::scene::SceneDescription& targetDesc = toAfter ? group.afterSubtree : group.beforeSubtree;
		const std::uint32_t targetParent = toAfter ? group.afterParent : group.beforeParent;
		const std::vector<std::uint32_t>& targetIds = toAfter ? group.afterIds : group.beforeIds;
		const std::vector<std::uint32_t>& otherIds = toAfter ? group.beforeIds : group.afterIds;

		// Remove entities that exist in the state we're leaving but not the target
		// (entities created inside this subtree by the edit being reverted).
		const std::unordered_set<std::uint32_t> targetSet(targetIds.begin(), targetIds.end());
		for (const std::uint32_t id: otherIds)
		{
			if (targetSet.contains(id))
			{
				continue;
			}
			const Entity entity{id};
			if (entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)))
			{
				ecs::DetachFromParent(world, entity);
				world.Destroy(entity);
			}
		}

		if (targetExists)
		{
			group.lastApplied = app::scene::RestoreSubtreeInPlace(targetDesc, world, app::scene::MakeApplySceneDeps(services), Entity{targetParent});
			group.lastAppliedDesc = &targetDesc;
		}
		else
		{
			group.lastApplied.clear();
			group.lastAppliedDesc = nullptr;
		}
	}

	void EntityDiffCommand::Undo(World& world, ServiceContainer& services)
	{
		for (Group& group: m_groups)
		{
			ApplyState(group, /*toAfter=*/false, world, services);
		}
	}

	void EntityDiffCommand::Redo(World& world, ServiceContainer& services)
	{
		for (Group& group: m_groups)
		{
			ApplyState(group, /*toAfter=*/true, world, services);
		}
	}

	Entity EntityDiffCommand::Remap(Entity entity) const
	{
		if (!entity.IsValid())
		{
			return entity;
		}
		for (const Group& group: m_groups)
		{
			if (group.lastAppliedDesc == nullptr)
			{
				continue;
			}
			for (std::size_t i = 0; i < group.lastAppliedDesc->entities.size() && i < group.lastApplied.size(); ++i)
			{
				if (group.lastAppliedDesc->entities[i].entityId == entity.id)
				{
					return group.lastApplied[i];
				}
			}
		}
		return entity;
	}
} // namespace aether::editor
