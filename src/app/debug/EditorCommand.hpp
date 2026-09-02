#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/TileMapAsset.hpp"
#include "debug/TilePaintingState.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class World;
	class ServiceContainer;
} // namespace aether

namespace aether::editor
{
	// A single reversible editor operation. Commands own the minimal data needed
	// to move the scene between its before/after states; Undo/Redo apply that data
	// in place, never tearing the scene down and rebuilding it.
	class IEditorCommand
	{
	public:
		virtual ~IEditorCommand() = default;

		virtual void Undo(World& world, ServiceContainer& services) = 0;
		virtual void Redo(World& world, ServiceContainer& services) = 0;

		[[nodiscard]] virtual std::string_view Label() const = 0;

		// After Undo/Redo runs, map an entity id captured before the op to the
		// handle it now lives under. Identity unless the op recreated entities
		// (which only happens when handles had already churned, e.g. across play).
		[[nodiscard]] virtual Entity Remap(Entity entity) const
		{
			return entity;
		}
	};

	// Typed transform edit (gizmo drag, inspector transform, control set_transform).
	// Stores only the affected entities' world matrices, so undo/redo set exactly
	// those transforms - no serialization, no other entity touched.
	class TransformCommand final : public IEditorCommand
	{
	public:
		struct Item
		{
			std::uint32_t id = 0;
			glm::mat4 before{1.0f};
			glm::mat4 after{1.0f};
		};

		explicit TransformCommand(std::vector<Item> items);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Transform";
		}

		[[nodiscard]] bool Empty() const
		{
			return m_items.empty();
		}

	private:
		std::vector<Item> m_items;
	};

	// One tilemap paint gesture (pencil stroke, rectangle, fill) as a main-history
	// command, so Ctrl+Z is one consistent stack across tiles and entities. Stores
	// the per-cell before/after and replays them through TileMapAsset::SetCell.
	class TileStrokeCommand final : public IEditorCommand
	{
	public:
		TileStrokeCommand(std::string tilemapPath, std::vector<TilePaintEdit> edits);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Paint tiles";
		}

		[[nodiscard]] bool Empty() const
		{
			return m_edits.empty();
		}

	private:
		void Apply(ServiceContainer& services, bool forward);

		std::string m_tilemapPath;
		std::vector<TilePaintEdit> m_edits;
	};

	// Deleting a tilemap layer takes every tile painted on it with it, so the whole layer is
	// captured by value. A TileMapLayer owns its chunks outright, which makes the copy a
	// complete record rather than a reference into something that is about to be erased.
	class RemoveTileLayerCommand final : public IEditorCommand
	{
	public:
		RemoveTileLayerCommand(std::string tilemapPath, std::size_t index, TileMapLayer layer);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Delete tile layer";
		}

	private:
		std::string m_tilemapPath;
		std::size_t m_index;
		TileMapLayer m_layer;
	};

	// A Sprite Animation panel edit (frames, durations, events, loop mode) as a
	// main-history command. Stores the before/after clip and applies it back
	// through a callback the panel supplies, so the global Ctrl+Z drives it with
	// no separate panel stack.
	class SpriteAnimationEditCommand final : public IEditorCommand
	{
	public:
		SpriteAnimationEditCommand(SpriteAnimationAsset before, SpriteAnimationAsset after, std::function<void(const SpriteAnimationAsset&)> apply);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Animation edit";
		}

	private:
		SpriteAnimationAsset m_before;
		SpriteAnimationAsset m_after;
		std::function<void(const SpriteAnimationAsset&)> m_apply;
	};

	// Typed create/delete of one or more entity subtrees. Stores the subtree
	// snapshot(s) and each root's parent id, and recreates through
	// RestoreSubtreeInPlace so undo of a delete brings entities back under their
	// original ids and parent - nothing else in the scene is touched.
	class SubtreeLifetimeCommand final : public IEditorCommand
	{
	public:
		// Capture the given roots (with descendants). createdByThisEdit=true means
		// the edit *created* them (undo destroys, redo recreates); false means the
		// edit is about to *delete* them (undo recreates, redo destroys) - call it
		// before the deletion runs. Returns nullptr if there's nothing to capture.
		static std::unique_ptr<SubtreeLifetimeCommand> Capture(World& world, ServiceContainer& services, const std::vector<Entity>& roots, bool createdByThisEdit, const char* label);

		SubtreeLifetimeCommand(app::scene::SceneDescription subtree, std::vector<std::uint32_t> parentIds, bool createdByThisEdit, const char* label);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return m_label;
		}

		[[nodiscard]] Entity Remap(Entity entity) const override;

	private:
		void Recreate(World& world, ServiceContainer& services);
		void DestroyAll(World& world);

		app::scene::SceneDescription m_subtree;
		std::vector<std::uint32_t> m_parentIds; // external parent id per record (only used where parentIndex < 0)
		bool m_createdByThisEdit = true;
		std::string_view m_label;
		std::vector<Entity> m_lastRecreated;
	};

	// Typed rename command for a single entity. Stores the entity id and
	// old/new names; undo/redo applies the name via EmplaceOrReplace.
	class RenameCommand final : public IEditorCommand
	{
	public:
		RenameCommand(std::uint32_t entityId, std::string oldName, std::string newName);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Rename";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_oldName;
		std::string m_newName;
	};

	// Typed reparent command for a single entity. Stores the entity id and
	// old/new parent ids; undo/redo calls ecs::SetParent.
	// Enabling or disabling an entity. Disabled is a tag with no fields, so neither the
	// reflected-field path nor add/remove-component modelled it, and the toggle recorded
	// nothing at all: the next Ctrl+Z reached straight past it into whatever came before.
	class SetEnabledCommand final : public IEditorCommand
	{
	public:
		SetEnabledCommand(std::uint32_t entityId, bool disabledBefore);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return m_disabledBefore ? "Enable" : "Disable";
		}

	private:
		std::uint32_t m_entityId;
		bool m_disabledBefore;
	};

	class ReparentCommand final : public IEditorCommand
	{
	public:
		ReparentCommand(std::uint32_t entityId, std::uint32_t oldParentId, std::uint32_t newParentId);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Reparent";
		}

	private:
		std::uint32_t m_entityId;
		std::uint32_t m_oldParentId;
		std::uint32_t m_newParentId;
	};

	// Typed add-component command. Stores the entity id and component catalog
	// name; undo removes the component, redo adds it back with features enabled.
	class AddComponentCommand final : public IEditorCommand
	{
	public:
		AddComponentCommand(std::uint32_t entityId, std::string componentName);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Add component";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_componentName;
	};

	// Typed remove-component command. Stores the entity id, component name,
	// and a JSON snapshot of the component's fields captured before removal.
	// Undo re-adds the component and restores its fields; redo removes it.
	class RemoveComponentCommand final : public IEditorCommand
	{
	public:
		RemoveComponentCommand(std::uint32_t entityId, std::string componentName, nlohmann::json snapshot, bool isReflected);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Remove component";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_componentName;
		nlohmann::json m_snapshot;
		bool m_isReflected;
	};

	// Typed set-component command. Stores the entity id, component name, and
	// before/after JSON snapshots of the fields that changed. Undo/Redo
	// applies the stored field values through the reflection or
	// hand-authored path.
	// Several commands produced by ONE user gesture, undone and redone as a single step.
	// An inspector drag over a multi-selection edits every selected entity; recorded
	// separately those cost the user one Ctrl+Z per entity and leave the scene visibly
	// half-reverted in between.
	class CompositeCommand final : public IEditorCommand
	{
	public:
		CompositeCommand(std::vector<std::unique_ptr<IEditorCommand>> commands, std::string label);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return m_label;
		}

		[[nodiscard]] Entity Remap(Entity entity) const override;

	private:
		std::vector<std::unique_ptr<IEditorCommand>> m_commands;
		std::string m_label;
	};

	class SetComponentCommand final : public IEditorCommand
	{
	public:
		SetComponentCommand(std::uint32_t entityId, std::string componentName, nlohmann::json before, nlohmann::json after, bool isReflected);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Set component";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_componentName;
		nlohmann::json m_before;
		nlohmann::json m_after;
		bool m_isReflected;
	};

	// Typed add-script command.  Stores the script type name and the full
	// ScriptEntry that was appended.  Undo removes it; Redo re-adds it.
	class AddScriptCommand final : public IEditorCommand
	{
	public:
		AddScriptCommand(std::uint32_t entityId, std::string scriptType, ScriptEntry entry);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Add script";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_scriptType;
		ScriptEntry m_entry;
	};

	// Typed remove-script command.  Stores the script type name and the full
	// ScriptEntry(s) that were removed.  Undo re-adds them; Redo removes.
	class RemoveScriptCommand final : public IEditorCommand
	{
	public:
		RemoveScriptCommand(std::uint32_t entityId, std::string scriptType, ScriptEntry entry);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Remove script";
		}

	private:
		std::uint32_t m_entityId;
		std::string m_scriptType;
		ScriptEntry m_entry;
	};

	// ── SceneReplaceCommand ─────────────────────────────────────────
	// Whole-scene replace (load / new).  Stores two full SceneDescription
	// snapshots and the associated scene name so undo restores the previous
	// scene and redo re-applies the replacement.
	class SceneReplaceCommand final : public IEditorCommand
	{
	public:
		SceneReplaceCommand(const app::scene::SceneDescription& before, const app::scene::SceneDescription& after, const std::string& beforeSceneName, const std::string& afterSceneName);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Scene replace";
		}

	private:
		void Apply(app::scene::SceneDescription& desc, const std::string& sceneName, World& world, ServiceContainer& services);

		app::scene::SceneDescription m_before;
		app::scene::SceneDescription m_after;
		std::string m_beforeSceneName;
		std::string m_afterSceneName;
	};

	// Typed unpack-prefab command. Unpacking strips a prefab instance's linkage
	// (PrefabInstanceComponent on the root, plus PrefabLinkComponent /
	// SceneTransientComponent across the expanded subtree) so the entities become
	// plain scene entities. Undo re-applies exactly the captured linkage in place -
	// no entity is destroyed or recreated, so ids and every other component survive;
	// Redo strips the linkage again.
	class UnpackPrefabCommand final : public IEditorCommand
	{
	public:
		// Snapshot the instance rooted at `root` before it is unpacked. Returns
		// nullptr if `root` is not a live prefab-instance root.
		static std::unique_ptr<UnpackPrefabCommand> Capture(World& world, Entity root);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Unpack prefab";
		}

	private:
		UnpackPrefabCommand() = default;

		// Per-entity linkage snapshot for one node of the expanded subtree.
		struct LinkRecord
		{
			std::uint32_t entityId = 0;
			bool hasLink = false;
			std::uint32_t instanceRoot = 0;
			std::uint64_t prefabGuid = 0;
			bool transient = false;
		};

		std::uint32_t m_rootId = 0;
		std::string m_prefabPath;
		std::vector<LinkRecord> m_records;
	};

	// Whole-ScriptComponent replace. The inspector's script drawer edits the script
	// type, its exposed properties, and slot lifetime through many separate widgets,
	// and ScriptComponent is not reflected - so the faithful unit of undo is the
	// entire before/after script list rather than a per-field or per-slot command.
	class SetScriptsCommand final : public IEditorCommand
	{
	public:
		SetScriptsCommand(std::uint32_t entityId, std::vector<ScriptEntry> before, std::vector<ScriptEntry> after);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Edit scripts";
		}

	private:
		void Apply(World& world, const std::vector<ScriptEntry>& scripts);

		std::uint32_t m_entityId;
		std::vector<ScriptEntry> m_before;
		std::vector<ScriptEntry> m_after;
	};

	// Tag membership for one entity. Tags live in a side table keyed by tag id rather
	// than as a reflected component, so a component-field snapshot cannot see them;
	// this stores the entity's whole tag set before and after.
	class SetTagsCommand final : public IEditorCommand
	{
	public:
		SetTagsCommand(std::uint32_t entityId, std::vector<std::uint32_t> before, std::vector<std::uint32_t> after);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Edit tags";
		}

	private:
		void Apply(World& world, const std::vector<std::uint32_t>& tags);

		std::uint32_t m_entityId;
		std::vector<std::uint32_t> m_before;
		std::vector<std::uint32_t> m_after;
	};

	// Compare two script lists by authored content (path + properties), ignoring the
	// runtime `attached` flag - which the script system flips on its own and would
	// otherwise register as a phantom edit every frame.
	[[nodiscard]] bool ScriptListsEqual(const std::vector<ScriptEntry>& a, const std::vector<ScriptEntry>& b);

	// Typed hierarchy move for the hierarchy panel's drag-and-drop: reparenting and
	// sibling reordering are the same operation (ecs::SetParent is InsertChildAt at
	// index -1). Stores each moved entity's old and new (parent, sibling index), so
	// undo restores both the original parent AND its original position among siblings
	// - which a parent-only command cannot express.
	class HierarchyMoveCommand final : public IEditorCommand
	{
	public:
		struct Item
		{
			std::uint32_t id = 0;
			std::uint32_t oldParent = 0;
			int oldIndex = -1;
			std::uint32_t newParent = 0;
			int newIndex = -1;
		};

		explicit HierarchyMoveCommand(std::vector<Item> items);

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Move in hierarchy";
		}

		[[nodiscard]] bool Empty() const
		{
			return m_items.empty();
		}

	private:
		void Apply(World& world, bool toAfter);

		std::vector<Item> m_items;
	};

	// Capture the editable fields of a component as a JSON object.  Returns true
	// if a snapshot was captured (component exists and has fields).  Used by
	// command handlers that need to snapshot before a mutation; callers that
	// always proceed (and handle an empty snapshot) may ignore the result.
	bool CaptureComponentFields(World& world, Entity entity, const std::string& type, ServiceContainer& services, nlohmann::json& out, bool& isReflected);

	// Apply a JSON field snapshot to a component through the reflected or
	// hand-authored path as indicated by isReflected.
	void ApplyComponentFields(World& world, Entity entity, const std::string& type, const nlohmann::json& values, bool isReflected, ServiceContainer& services);
} // namespace aether::editor
