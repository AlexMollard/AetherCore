#include "debug/EditorCommand.hpp"
#include "material/EffectManager.hpp"
#include "material/MaterialRegistry.hpp"
#include "scripting/SceneContext.hpp"
#include "material/MaterialSystem.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/AssetManager.hpp"
#include "assets/TileAssetStore.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/ComponentFields.hpp"
#include "editor/ReflectionJson.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Components.hpp"
#include "ui/UiComponents.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/World.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/reflection/Reflection.hpp"
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
	      : m_before(std::move(before)), m_after(std::move(after)), m_apply(std::move(apply))
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
	      : m_tilemapPath(std::move(tilemapPath)), m_edits(std::move(edits))
	{
	}

	RemoveEffectCommand::RemoveEffectCommand(const std::uint32_t entityId, std::string effectName, EffectParams params)
	      : m_entityId(entityId), m_effectName(std::move(effectName)), m_params(params)
	{
	}

	void RemoveEffectCommand::Undo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto* sceneCtx = services.TryGet<app::scripting::SceneContext>();
		auto* assets = services.TryGet<AssetManager>();
		if (sceneCtx == nullptr || sceneCtx->effects == nullptr || assets == nullptr)
		{
			return;
		}
		// Goes through the same entry point the inspector uses, so the parameter slot and the
		// pipeline are assigned the way they would be for a fresh apply.
		(void) effects::ApplyEntityEffect(world, entity, m_effectName, *sceneCtx->effects, assets->GetPipelineCache(), assets->GetEffectParamBuffer(), &m_params);
	}

	void RemoveEffectCommand::Redo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		world.Remove<EffectParamsComponent>(entity);
		world.Remove<EffectRefComponent>(entity);
		// Put the plain material back if there is one, matching what the panel's X does.
		auto* assets = services.TryGet<AssetManager>();
		const auto* mc = world.TryGet<MaterialComponent>(entity);
		MaterialAsset asset{};
		if (assets != nullptr && mc != nullptr && assets->GetMaterialRegistry().TryDescribe(mc->handle, asset))
		{
			MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
		}
		else
		{
			world.Remove<PipelineComponent>(entity);
		}
	}

AddEffectCommand::AddEffectCommand(const std::uint32_t entityId, std::string effectName, EffectParams params)
      : m_entityId(entityId), m_effectName(std::move(effectName)), m_params(params)
{
}

void AddEffectCommand::Undo(World& world, ServiceContainer& services)
{
	const Entity entity{m_entityId};
	if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
	{
		return;
	}
	world.Remove<EffectParamsComponent>(entity);
	world.Remove<EffectRefComponent>(entity);
	// Put the plain material back if there is one, matching what the panel's X does.
	auto* assets = services.TryGet<AssetManager>();
	const auto* mc = world.TryGet<MaterialComponent>(entity);
	MaterialAsset asset{};
	if (assets != nullptr && mc != nullptr && assets->GetMaterialRegistry().TryDescribe(mc->handle, asset))
	{
		MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
	}
	else
	{
		world.Remove<PipelineComponent>(entity);
	}
}

void AddEffectCommand::Redo(World& world, ServiceContainer& services)
{
	const Entity entity{m_entityId};
	if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
	{
		return;
	}
	auto* sceneCtx = services.TryGet<app::scripting::SceneContext>();
	auto* assets = services.TryGet<AssetManager>();
	if (sceneCtx == nullptr || sceneCtx->effects == nullptr || assets == nullptr)
	{
		return;
	}
	// Goes through the same entry point the inspector uses, so the parameter slot and the
	// pipeline are assigned the way they would be for a fresh apply.
	(void) effects::ApplyEntityEffect(world, entity, m_effectName, *sceneCtx->effects, assets->GetPipelineCache(), assets->GetEffectParamBuffer(), &m_params);
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

	RemoveTileLayerCommand::RemoveTileLayerCommand(std::string tilemapPath, const std::size_t index, TileMapLayer layer)
	      : m_tilemapPath(std::move(tilemapPath)), m_index(index), m_layer(std::move(layer))
	{
	}

	void RemoveTileLayerCommand::Undo(World&, ServiceContainer& services)
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
		// Clamped rather than asserted: layers may have been added or removed since, and
		// putting this one back at the end beats dropping it on the floor.
		const std::size_t at = std::min(m_index, map->layers.size());
		map->layers.insert(map->layers.begin() + static_cast<std::ptrdiff_t>(at), m_layer);
		map->dirty = true;
	}

	void RemoveTileLayerCommand::Redo(World&, ServiceContainer& services)
	{
		auto* tiles = services.TryGet<TileAssetStore>();
		if (tiles == nullptr)
		{
			return;
		}
		TileMapAsset* map = tiles->MutableTileMap(m_tilemapPath);
		if (map == nullptr || m_index >= map->layers.size())
		{
			return;
		}
		map->layers.erase(map->layers.begin() + static_cast<std::ptrdiff_t>(m_index));
		map->dirty = true;
	}

	UiRectCommand::UiRectCommand(std::vector<Item> items) : m_items(std::move(items))
	{
	}

	namespace
	{
		void ApplyUiRect(World& world, std::uint32_t id, glm::vec2 offsetMin, glm::vec2 offsetMax)
		{
			const Entity entity{id};
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				return;
			}
			if (auto* rect = world.TryGet<ui::UIRect>(entity))
			{
				rect->offsetMin = offsetMin;
				rect->offsetMax = offsetMax;
			}
		}
	} // namespace

	void UiRectCommand::Undo(World& world, ServiceContainer&)
	{
		for (const Item& item: m_items)
		{
			ApplyUiRect(world, item.id, item.beforeMin, item.beforeMax);
		}
	}

	void UiRectCommand::Redo(World& world, ServiceContainer&)
	{
		for (const Item& item: m_items)
		{
			ApplyUiRect(world, item.id, item.afterMin, item.afterMax);
		}
	}

	AddTileLayerCommand::AddTileLayerCommand(std::string tilemapPath, const std::size_t index, TileMapLayer layer)
	      : m_tilemapPath(std::move(tilemapPath)), m_index(index), m_layer(std::move(layer))
	{
	}

	void AddTileLayerCommand::Undo(World&, ServiceContainer& services)
	{
		auto* tiles = services.TryGet<TileAssetStore>();
		if (tiles == nullptr)
		{
			return;
		}
		TileMapAsset* map = tiles->MutableTileMap(m_tilemapPath);
		if (map == nullptr || m_index >= map->layers.size())
		{
			return;
		}
		// Re-captured before erasing, so a redo puts back whatever the layer actually held
		// rather than the empty one it was created as.
		m_layer = map->layers[m_index];
		map->layers.erase(map->layers.begin() + static_cast<std::ptrdiff_t>(m_index));
		map->dirty = true;
	}

	void AddTileLayerCommand::Redo(World&, ServiceContainer& services)
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
		// Clamped for the same reason the remove command clamps: layers may have moved since.
		const std::size_t at = std::min(m_index, map->layers.size());
		map->layers.insert(map->layers.begin() + static_cast<std::ptrdiff_t>(at), m_layer);
		map->dirty = true;
	}

	void TileStrokeCommand::Undo(World&, ServiceContainer& services)
	{
		Apply(services, /*forward=*/false);
	}

	void TileStrokeCommand::Redo(World&, ServiceContainer& services)
	{
		Apply(services, /*forward=*/true);
	}

	CompositeCommand::CompositeCommand(std::vector<std::unique_ptr<IEditorCommand>> commands, std::string label)
	    : m_commands(std::move(commands)), m_label(std::move(label))
	{
	}

	void CompositeCommand::Undo(World& world, ServiceContainer& services)
	{
		// Reverse order: the parts were applied front-to-back, so unwinding back-to-front
		// is what makes a composite behave like the single step it represents.
		for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
		{
			(*it)->Undo(world, services);
		}
	}

	void CompositeCommand::Redo(World& world, ServiceContainer& services)
	{
		for (const auto& command: m_commands)
		{
			command->Redo(world, services);
		}
	}

	Entity CompositeCommand::Remap(Entity entity) const
	{
		for (const auto& command: m_commands)
		{
			entity = command->Remap(entity);
		}
		return entity;
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
	      : m_subtree(std::move(subtree)), m_parentIds(std::move(parentIds)), m_createdByThisEdit(createdByThisEdit), m_label(label)
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

	// ── RenameCommand ────────────────────────────────────────────────

	RenameCommand::RenameCommand(std::uint32_t entityId, std::string oldName, std::string newName)
	      : m_entityId(entityId), m_oldName(std::move(oldName)), m_newName(std::move(newName))
	{
	}

	void RenameCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		world.EmplaceOrReplace<NameComponent>(entity, NameComponent{.name = m_oldName});
	}

	void RenameCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		world.EmplaceOrReplace<NameComponent>(entity, NameComponent{.name = m_newName});
	}

	// ── SetEnabledCommand ────────────────────────────────────────────

	SetEnabledCommand::SetEnabledCommand(std::uint32_t entityId, bool disabledBefore)
	    : m_entityId(entityId), m_disabledBefore(disabledBefore)
	{
	}

	namespace
	{
		void ApplyDisabled(World& world, Entity entity, bool disabled)
		{
			if (!world.GetRegistry().valid(World::ToEntt(entity)))
			{
				return;
			}
			if (disabled)
			{
				world.EmplaceOrReplace<DisabledComponent>(entity);
			}
			else if (world.Has<DisabledComponent>(entity))
			{
				world.Remove<DisabledComponent>(entity);
			}
		}
	} // namespace

	void SetEnabledCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		ApplyDisabled(world, Entity{m_entityId}, m_disabledBefore);
	}

	void SetEnabledCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		ApplyDisabled(world, Entity{m_entityId}, !m_disabledBefore);
	}

	// ── ReparentCommand ──────────────────────────────────────────────

	ReparentCommand::ReparentCommand(std::uint32_t entityId, std::uint32_t oldParentId, std::uint32_t newParentId)
	      : m_entityId(entityId), m_oldParentId(oldParentId), m_newParentId(newParentId)
	{
	}

	void ReparentCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		aether::ecs::SetParent(world, Entity{m_entityId}, Entity{m_oldParentId});
	}

	void ReparentCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		aether::ecs::SetParent(world, Entity{m_entityId}, Entity{m_newParentId});
	}

	// ── Component command helpers ──────────────────────────────────────

	bool AddComponentTo(World& world, Entity entity, const std::string& type, ServiceContainer& services)
	{
		if (const ComponentCatalogEntry* entry = FindComponent(type); entry != nullptr && entry->add)
		{
			entry->add(world, entity, services);
			EnableComponentFeatures(world, *entry);
			return true;
		}
		if (const reflect::ComponentType* rt = reflect::FindComponentType(type); rt != nullptr && rt->emplaceDefault)
		{
			(void) rt->emplaceDefault(world, entity);
			return true;
		}
		return false;
	}

	bool RemoveComponentFrom(World& world, Entity entity, const std::string& type)
	{
		if (const ComponentCatalogEntry* entry = FindComponent(type); entry != nullptr && entry->remove)
		{
			entry->remove(world, entity);
			return true;
		}
		if (const reflect::ComponentType* rt = reflect::FindComponentType(type); rt != nullptr && rt->remove)
		{
			rt->remove(world, entity);
			return true;
		}
		return false;
	}

	bool CaptureComponentFields(World& world, Entity entity, const std::string& type, ServiceContainer& services, nlohmann::json& out, bool& isReflected)
	{
		if (const auto* rt = reflect::FindComponentType(type))
		{
			const void* comp = rt->tryGetRawConst(world, entity);
			if (comp == nullptr)
			{
				return false;
			}
			isReflected = true;
			nlohmann::json fields = nlohmann::json::object();
			for (const auto& f: rt->fields)
			{
				const reflect::FieldValue fv = f.get(comp);
				if (f.type == reflect::FieldType::Enum && f.meta.enumTable != nullptr)
				{
					fields[f.name] = f.meta.enumTable->NameOf(fv.enumValue);
				}
				else
				{
					fields[f.name] = editor::FieldValueToJson(fv, &f);
				}
			}
			out = std::move(fields);
			return true;
		}
		const auto* fields = editor::FindComponentFields(type);
		if (fields == nullptr)
		{
			return false;
		}
		isReflected = false;
		nlohmann::json snapshot = nlohmann::json::object();
		if (!fields->read(world, entity, services, snapshot))
		{
			return false;
		}
		out = std::move(snapshot);
		return true;
	}

	void ApplyComponentFields(World& world, Entity entity, const std::string& type, const nlohmann::json& values, bool isReflected, ServiceContainer& services)
	{
		if (isReflected)
		{
			if (const auto* rt = reflect::FindComponentType(type))
			{
				void* comp = rt->tryGetRaw(world, entity);
				if (comp == nullptr)
				{
					return;
				}
				for (const auto& f: rt->fields)
				{
					if (values.contains(f.name))
					{
						f.set(comp, editor::JsonToFieldValue(values.at(f.name), f));
					}
				}
				if (rt->postSet)
				{
					rt->postSet(world, entity);
				}
			}
			return;
		}
		const auto* fields = editor::FindComponentFields(type);
		if (fields != nullptr)
		{
			fields->write(world, entity, values, services);
		}
	}

	// ── AddComponentCommand ────────────────────────────────────────────

	AddComponentCommand::AddComponentCommand(std::uint32_t entityId, std::string componentName)
	      : m_entityId(entityId), m_componentName(std::move(componentName))
	{
	}

	void AddComponentCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		(void) RemoveComponentFrom(world, entity, m_componentName);
	}

	void AddComponentCommand::Redo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		(void) AddComponentTo(world, entity, m_componentName, services);
	}

	// ── RemoveComponentCommand ─────────────────────────────────────────

	RemoveComponentCommand::RemoveComponentCommand(std::uint32_t entityId, std::string componentName, nlohmann::json snapshot, bool isReflected)
	      : m_entityId(entityId), m_componentName(std::move(componentName)), m_snapshot(std::move(snapshot)), m_isReflected(isReflected)
	{
	}

	void RemoveComponentCommand::Undo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		// The component has to exist again before its values can go back: ApplyComponentFields
		// gives up on a missing component.
		(void) AddComponentTo(world, entity, m_componentName, services);
		if (!m_snapshot.empty())
		{
			ApplyComponentFields(world, entity, m_componentName, m_snapshot, m_isReflected, services);
		}
	}

	void RemoveComponentCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		(void) RemoveComponentFrom(world, entity, m_componentName);
	}

	// ── SetComponentCommand ────────────────────────────────────────────

	SetComponentCommand::SetComponentCommand(std::uint32_t entityId, std::string componentName, nlohmann::json before, nlohmann::json after, bool isReflected)
	      : m_entityId(entityId), m_componentName(std::move(componentName)), m_before(std::move(before)), m_after(std::move(after)), m_isReflected(isReflected)
	{
	}

	void SetComponentCommand::Undo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		ApplyComponentFields(world, entity, m_componentName, m_before, m_isReflected, services);
	}

	void SetComponentCommand::Redo(World& world, ServiceContainer& services)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		ApplyComponentFields(world, entity, m_componentName, m_after, m_isReflected, services);
	}

	// ── AddScriptCommand ────────────────────────────────────────────────

	AddScriptCommand::AddScriptCommand(std::uint32_t entityId, std::string scriptType, ScriptEntry entry)
	      : m_entityId(entityId), m_scriptType(std::move(scriptType)), m_entry(std::move(entry))
	{
	}

	void AddScriptCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			return;
		}
		// Remove the last script whose path matches the type.
		for (auto it = sc->scripts.begin(); it != sc->scripts.end(); ++it)
		{
			if (it->path == m_scriptType)
			{
				sc->scripts.erase(it);
				break;
			}
		}
	}

	void AddScriptCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		// Append to whatever is there. EmplaceOrReplace default-constructs, so re-adding one
		// script used to wipe every OTHER script on the entity.
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			sc = &world.Emplace<ScriptComponent>(entity);
		}
		sc->scripts.push_back(m_entry);
	}

	// ── RemoveScriptCommand ────────────────────────────────────────────

	RemoveScriptCommand::RemoveScriptCommand(std::uint32_t entityId, std::string scriptType, ScriptEntry entry)
	      : m_entityId(entityId), m_scriptType(std::move(scriptType)), m_entry(std::move(entry))
	{
	}

	void RemoveScriptCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		// Same as the add path: restoring one script must not discard the others. Redo below
		// erases only the matching entry, so an Undo that replaced the whole component was
		// asymmetric with it and lost work.
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			sc = &world.Emplace<ScriptComponent>(entity);
		}
		sc->scripts.push_back(m_entry);
	}

	void RemoveScriptCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			return;
		}
		std::erase_if(sc->scripts, [this](const ScriptEntry& s) { return s.path == m_scriptType; });
	}

	// ── SceneReplaceCommand ────────────────────────────────────────────────

	SceneReplaceCommand::SceneReplaceCommand(const app::scene::SceneDescription& before, const app::scene::SceneDescription& after, const std::string& beforeSceneName, const std::string& afterSceneName)
	      : m_before(before), m_after(after), m_beforeSceneName(beforeSceneName), m_afterSceneName(afterSceneName)
	{
	}

	void SceneReplaceCommand::Apply(app::scene::SceneDescription& desc, const std::string& sceneName, World& world, ServiceContainer& services)
	{
		auto deps = app::scene::MakeApplySceneDeps(services);
		app::scene::ReplaceScene(desc, world, deps);
		auto* scenes = services.TryGet<SceneSubsystem>();
		if (scenes != nullptr)
		{
			scenes->SetCurrentScene(sceneName);
		}
	}

	void SceneReplaceCommand::Undo(World& world, ServiceContainer& services)
	{
		Apply(m_before, m_beforeSceneName, world, services);
	}

	void SceneReplaceCommand::Redo(World& world, ServiceContainer& services)
	{
		Apply(m_after, m_afterSceneName, world, services);
	}

	// ── UnpackPrefabCommand ─────────────────────────────────────────────

	std::unique_ptr<UnpackPrefabCommand> UnpackPrefabCommand::Capture(World& world, Entity root)
	{
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			return nullptr;
		}
		const auto* inst = world.TryGet<PrefabInstanceComponent>(root);
		if (inst == nullptr)
		{
			return nullptr;
		}
		auto cmd = std::unique_ptr<UnpackPrefabCommand>(new UnpackPrefabCommand());
		cmd->m_rootId = root.id;
		cmd->m_prefabPath = inst->prefabPath;

		// Walk the subtree in the same order the unpack handler does, snapshotting
		// each node's prefab linkage so undo can restore it exactly.
		std::vector<Entity> subtree{root};
		for (std::size_t i = 0; i < subtree.size(); ++i)
		{
			if (const auto* h = world.TryGet<HierarchyComponent>(subtree[i]))
			{
				subtree.insert(subtree.end(), h->children.begin(), h->children.end());
			}
		}
		cmd->m_records.reserve(subtree.size());
		for (const Entity e: subtree)
		{
			LinkRecord rec;
			rec.entityId = e.id;
			if (const auto* link = world.TryGet<PrefabLinkComponent>(e))
			{
				rec.hasLink = true;
				rec.instanceRoot = link->instanceRoot.id;
				rec.prefabGuid = link->prefabGuid;
			}
			rec.transient = world.Has<SceneTransientComponent>(e);
			cmd->m_records.push_back(rec);
		}
		return cmd;
	}

	void UnpackPrefabCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		const Entity root{m_rootId};
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			return;
		}
		world.EmplaceOrReplace<PrefabInstanceComponent>(root, PrefabInstanceComponent{.prefabPath = m_prefabPath});
		for (const LinkRecord& rec: m_records)
		{
			const Entity e{rec.entityId};
			if (!world.GetRegistry().valid(World::ToEntt(e)))
			{
				continue;
			}
			if (rec.hasLink)
			{
				world.EmplaceOrReplace<PrefabLinkComponent>(e, PrefabLinkComponent{.instanceRoot = Entity{rec.instanceRoot}, .prefabGuid = rec.prefabGuid});
			}
			if (rec.transient)
			{
				world.EmplaceOrReplace<SceneTransientComponent>(e);
			}
		}
	}

	void UnpackPrefabCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		const Entity root{m_rootId};
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			return;
		}
		for (const LinkRecord& rec: m_records)
		{
			const Entity e{rec.entityId};
			if (!world.GetRegistry().valid(World::ToEntt(e)))
			{
				continue;
			}
			world.Remove<PrefabLinkComponent>(e);
			world.Remove<SceneTransientComponent>(e);
		}
		world.Remove<PrefabInstanceComponent>(root);
	}

	// ── SetScriptsCommand ───────────────────────────────────────────────

	namespace
	{
		bool ScriptPropertiesEqual(const ScriptPropertyValue& a, const ScriptPropertyValue& b)
		{
			return a.type == b.type && a.i64 == b.i64 && a.str == b.str && a.f4[0] == b.f4[0] && a.f4[1] == b.f4[1] && a.f4[2] == b.f4[2] && a.f4[3] == b.f4[3];
		}
	} // namespace

	bool ScriptListsEqual(const std::vector<ScriptEntry>& a, const std::vector<ScriptEntry>& b)
	{
		if (a.size() != b.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i)
		{
			if (a[i].path != b[i].path || a[i].properties.size() != b[i].properties.size())
			{
				return false;
			}
			for (const auto& [key, value]: a[i].properties)
			{
				const auto it = b[i].properties.find(key);
				if (it == b[i].properties.end() || !ScriptPropertiesEqual(value, it->second))
				{
					return false;
				}
			}
		}
		return true;
	}

	SetScriptsCommand::SetScriptsCommand(std::uint32_t entityId, std::vector<ScriptEntry> before, std::vector<ScriptEntry> after)
	      : m_entityId(entityId), m_before(std::move(before)), m_after(std::move(after))
	{
	}

	void SetScriptsCommand::Apply(World& world, const std::vector<ScriptEntry>& scripts)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto& sc = world.EmplaceOrReplace<ScriptComponent>(entity);
		sc.scripts = scripts;
		// The managed instances behind the previous list are gone; let the script
		// system re-attach rather than trusting a stale flag.
		for (ScriptEntry& script: sc.scripts)
		{
			script.attached = false;
		}
	}

	void SetScriptsCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, m_before);
	}

	void SetScriptsCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, m_after);
	}

	// ── SetTagsCommand ──────────────────────────────────────────────────

	SetTagsCommand::SetTagsCommand(std::uint32_t entityId, std::vector<std::uint32_t> before, std::vector<std::uint32_t> after)
	      : m_entityId(entityId), m_before(std::move(before)), m_after(std::move(after))
	{
	}

	void SetTagsCommand::Apply(World& world, const std::vector<std::uint32_t>& tags)
	{
		const Entity entity{m_entityId};
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		// Restore exact membership: every known tag is either added or dropped, so a
		// tag added since capture is cleared rather than left behind.
		ForEachTag(
		        [&](const std::string&, std::uint32_t tagId)
		        {
			        if (std::find(tags.begin(), tags.end(), tagId) != tags.end())
			        {
				        TagAdd(&world, m_entityId, tagId);
			        }
			        else
			        {
				        TagRemove(&world, m_entityId, tagId);
			        }
		        });
	}

	void SetTagsCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, m_before);
	}

	void SetTagsCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, m_after);
	}

	// ── HierarchyMoveCommand ────────────────────────────────────────────

	HierarchyMoveCommand::HierarchyMoveCommand(std::vector<Item> items)
	      : m_items(std::move(items))
	{
	}

	void HierarchyMoveCommand::Apply(World& world, bool toAfter)
	{
		// Re-insert in ascending target index: rebuilding a sibling list one entity at
		// a time only lands each at its recorded position if earlier slots are filled
		// first, so capture order must not matter here.
		std::vector<std::size_t> order(m_items.size());
		for (std::size_t i = 0; i < order.size(); ++i)
		{
			order[i] = i;
		}
		std::stable_sort(order.begin(), order.end(), [this, toAfter](std::size_t a, std::size_t b) { return (toAfter ? m_items[a].newIndex : m_items[a].oldIndex) < (toAfter ? m_items[b].newIndex : m_items[b].oldIndex); });

		for (const std::size_t i: order)
		{
			const Item& item = m_items[i];
			const Entity entity{item.id};
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				continue;
			}
			// id 0 means "no parent" (a scene root); a parent that no longer exists
			// cannot take the child back, so leave that entity where it is.
			const Entity parent{toAfter ? item.newParent : item.oldParent};
			if (parent.IsValid() && !world.GetRegistry().valid(World::ToEntt(parent)))
			{
				continue;
			}
			ecs::InsertChildAt(world, entity, parent, toAfter ? item.newIndex : item.oldIndex);
		}
	}

	void HierarchyMoveCommand::Undo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, /*toAfter=*/false);
	}

	void HierarchyMoveCommand::Redo(World& world, ServiceContainer& /*services*/)
	{
		Apply(world, /*toAfter=*/true);
	}
} // namespace aether::editor
