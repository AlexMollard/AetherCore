#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/SpriteAnimationAsset.hpp"
#include "debug/TilePaintingState.hpp"
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

	// Generic surgical diff of two whole-scene snapshots. Diffs them into the set of
	// affected top-level subtrees (created / deleted / edited) and, on undo/redo,
	// only creates, destroys or restores THOSE subtrees - every unchanged entity
	// (and its transient runtime state) is left untouched. Replaces the whole-scene
	// restore for any edit that isn't already a dedicated typed command.
	class EntityDiffCommand final : public IEditorCommand
	{
	public:
		EntityDiffCommand(const app::scene::SceneDescription& before, const app::scene::SceneDescription& after);

		[[nodiscard]] bool Empty() const
		{
			return m_groups.empty();
		}

		void Undo(World& world, ServiceContainer& services) override;
		void Redo(World& world, ServiceContainer& services) override;

		[[nodiscard]] std::string_view Label() const override
		{
			return "Edit";
		}

		[[nodiscard]] Entity Remap(Entity entity) const override;

	private:
		// One affected top-level subtree, captured in whichever states it exists.
		struct Group
		{
			bool existsBefore = false;
			bool existsAfter = false;
			app::scene::SceneDescription beforeSubtree;
			app::scene::SceneDescription afterSubtree;
			std::uint32_t beforeParent = 0;
			std::uint32_t afterParent = 0;
			std::vector<std::uint32_t> beforeIds;
			std::vector<std::uint32_t> afterIds;
			std::vector<Entity> lastApplied;
			const app::scene::SceneDescription* lastAppliedDesc = nullptr;
		};

		void ApplyState(Group& group, bool toAfter, World& world, ServiceContainer& services);

		std::vector<Group> m_groups;
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
} // namespace aether::editor
