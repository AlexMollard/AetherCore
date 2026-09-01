#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "debug/SceneSelection.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class TomlConfig;
	class World;
} // namespace aether

namespace aether::editor
{
	class UndoStack;

	class HierarchyPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Scene";
		}


		// Part of the default layout: what is in the scene.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

		void RequestSaveAsPopup();
		void RequestOpenPopup();

	private:
		struct FlatTreeEntry
		{
			Entity entity;
			int depth;
			std::uint64_t openMask;
			// How many identically named siblings this row stands for. 1 is an ordinary row.
			// A spawner that makes 40 "Orb" entities turns the hierarchy into 40 rows saying
			// the same word, which buries everything you actually authored.
			int runLength = 1;
		};

		enum class DropZone : std::uint8_t
		{
			Before,
			Inside,
			After
		};
		enum class KeyboardFocusScope : std::uint8_t
		{
			None,
			SceneList
		};

		struct PendingReparent
		{
			Entity child;
			Entity target;
			DropZone zone;
		};

		// How many consecutive siblings starting at `start` share a name and are leaves, and
		// whether that run is long enough to fold into one row. Shared by the root list and by
		// every child list, so both collapse by the same rule.
		[[nodiscard]] std::size_t IdenticalRunAt(World& world, const std::vector<Entity>& siblings, std::size_t start) const;
		[[nodiscard]] bool ShouldCollapseRun(const std::vector<Entity>& siblings, std::size_t start, std::size_t run) const;

		void DrawNode(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, int depth, int flatTreeIndex, bool searching, std::string_view needle);
		void FlattenNode(World& world, Entity e, int depth, std::uint64_t openMask);
		void DrawRowBackdrop(const SceneSelection& selection, Entity e, int rowIndex);
		// `undo` may be null; the inline rename still applies, it just leaves no history.
		void DrawRowContent(World& world, Entity e, bool searching, std::string_view needle, UndoStack* undo, bool continuePreviousItem = true, int runLength = 1);
		void HandleRowClick(SceneSelection& selection, Entity e);
		void HandleRowDragDrop(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, float dropMinY, float dropMaxY, float visualMaxX);
		// Returns true if the menu destroyed `e` (callers must not touch it after).
		bool DrawRowContextMenu(app::LayerContext& context, World& world, SceneSelection& selection, Entity e);
		void BeginRename(const World& world, Entity e);
		// `undo` may be null (no editor undo stack); the toggles still apply.
		void DrawRowUtilityToggles(World& world, Entity e, UndoStack* undo);
		// Wrap the selection (or `fallback`) in a fresh empty parent, as one undo step.
		void GroupSelectionUnderNewParent(app::LayerContext& context, World& world, SceneSelection& selection, Entity fallback);
		void DrawBreadcrumbTrail(const World& world, SceneSelection& selection);
		void DrawTreeGuideLines(ImDrawList* drawList, const FlatTreeEntry& entry, const ImVec2& rowMin, const ImVec2& rowMax) const;
		void UpdateKeyboardFocusScopeFromMouse(const ImVec2& sceneListMin, const ImVec2& sceneListMax);
		bool SceneListOwnsKeyboard() const noexcept;
		void HandleKeyboardNavigation(SceneSelection& selection);
		void HandleTypeToJump(const World& world, SceneSelection& selection);
		void DrawOpenSceneModal(app::LayerContext& context, World& world, SceneSelection& selection);

		// Open Scene dialog: metadata scanned once per open/refresh.
		struct SceneListEntry
		{
			std::string name;
			std::string kindLabel;
			std::size_t entityCount = 0;
		};

		char m_search[64] = {};
		char m_sceneNameBuf[48] = "scene";
		std::vector<std::string> m_sceneList;
		std::vector<SceneListEntry> m_sceneEntries;
		char m_sceneSearch[64] = {};
		std::string m_openSceneSelected;
		bool m_sceneSearchFocusPending = false;
		bool m_sceneListDirty = true;
		bool m_requestSaveAsPopup = false;
		bool m_requestOpenPopup = false;
		Entity m_prefabSaveTarget{};
		char m_prefabNameBuf[48] = "";
		bool m_openPrefabSave = false;
		// Existing file names, snapshotted when a save modal appears (overwrite warnings).
		std::vector<std::string> m_saveExistingNames;
		bool m_filterMesh = false;
		bool m_filterSkinned = false;
		bool m_filterPhysics = false;
		bool m_filterEffect = false;

		Entity m_renaming{};
		char m_renameBuf[64] = {};
		bool m_renameFocusPending = false;

		std::vector<Entity> m_rowsCur;
		std::vector<Entity> m_rowsPrev;
		std::vector<Entity> m_filteredRowsScratch;
		std::vector<FlatTreeEntry> m_flatTree;
		std::unordered_set<std::uint32_t> m_expandedNodes;
		// Runs the user has opened up, keyed by the first entity in the run.
		std::unordered_set<std::uint32_t> m_expandedRuns;
		// Drag-hover spring-loading: hovering a collapsed row mid-drag opens it, so a subtree
		// can be dropped into without breaking the drag to expand it first.
		std::uint32_t m_dragHoverEntity = 0;
		float m_dragHoverSeconds = 0.0f;
		Entity m_rangeAnchor{};
		Entity m_pendingCollapse{};
		Entity m_pendingClick{};
		bool m_pendingClickCtrl = false;
		bool m_pendingClickShift = false;

		std::optional<PendingReparent> m_pendingReparent;
		bool m_pendingDuplicate = false;
		bool m_pendingCopy = false;
		bool m_pendingCut = false;
		bool m_pendingPaste = false;

		std::unordered_set<std::uint32_t> m_knownIds;
		std::unordered_set<std::uint32_t> m_knownIdsScratch;
		std::unordered_map<std::uint32_t, double> m_spawnFlash;
		bool m_knownSeeded = false;
		std::uint64_t m_seenSelectionSerial = 0;
		double m_pulseStart = -1.0;

		bool m_dirty = false;

		int m_focusedRowIndex = -1;
		KeyboardFocusScope m_keyboardFocusScope = KeyboardFocusScope::None;

		std::string m_typeJumpText;
		double m_typeJumpTime = -1.0;

		Entity m_scrollToEntity{};

	};
} // namespace aether::editor
