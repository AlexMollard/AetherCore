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

namespace aether::app
{
	// The scene outliner. Draws the window titled "Scene" (the saved dock-node
	// mapping keys off the window title, so the title stays even though the
	// panel class replaced InspectorPanel's old flat list).
	class HierarchyPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Scene Outliner";
		}

		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

		// Drives this panel's existing Save-As / Open (load) popups from
		// outside (the editor's File menu and Ctrl+S), so there is one popup
		// implementation instead of a duplicate. Both force the panel visible
		// since the popup only draws while this window does.
		void RequestSaveAsPopup();
		void RequestOpenPopup();

	private:
		// Flattened tree entry with guide-line open-mask bits.
		struct FlatTreeEntry
		{
			Entity entity;
			int depth;
			std::uint64_t openMask; // bit d set = more nodes at depth d follow
		};

		// Tri-zone drag-drop target zone.
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

		// Pending reparent (with sibling-reorder zone info).
		struct PendingReparent
		{
			Entity child;
			Entity target;
			DropZone zone;
		};

		void DrawNode(LayerContext& context, World& world, SceneSelection& selection, Entity e, int depth, int flatTreeIndex, bool searching, std::string_view needle);
		void FlattenNode(World& world, Entity e, int depth, std::uint64_t openMask);
		void DrawRowBackdrop(const SceneSelection& selection, Entity e, int rowIndex);
		void DrawRowContent(World& world, Entity e, bool searching, std::string_view needle, bool continuePreviousItem = true);
		void HandleRowClick(SceneSelection& selection, Entity e);
		// Drag source + drop target for one row (tree node or flat Selectable).
		void HandleRowDragDrop(LayerContext& context, World& world, SceneSelection& selection, Entity e, float dropMinY, float dropMaxY, float visualMaxX);
		// Returns true if the menu destroyed `e` (callers must not touch it after).
		bool DrawRowContextMenu(World& world, SceneSelection& selection, Entity e);
		void BeginRename(const World& world, Entity e);
		void DrawRowUtilityToggles(World& world, Entity e);
		std::string ComputeEntityPath(const World& world, Entity e) const;
		void SyncExpandedFromPaths(World& world);
		void DrawBreadcrumbTrail(const World& world, SceneSelection& selection);
		void RefreshAssetLists();
		void DrawAssetBrowser(LayerContext& context, World& world, SceneSelection& selection);
		void DrawAssetRow(LayerContext& context, World& world, SceneSelection& selection, SceneSelection::AssetKind kind, const std::string& path, const std::string& displayName, const char* icon);
		void DrawTreeGuideLines(ImDrawList* drawList, const FlatTreeEntry& entry, const ImVec2& rowMin, const ImVec2& rowMax) const;
		void UpdateKeyboardFocusScopeFromMouse(const ImVec2& sceneListMin, const ImVec2& sceneListMax);
		bool SceneListOwnsKeyboard() const noexcept;
		void HandleKeyboardNavigation(SceneSelection& selection);
		void HandleTypeToJump(const World& world, SceneSelection& selection);

		char m_search[64] = {};
		// Scene save/load popups.
		char m_sceneNameBuf[48] = "scene";
		std::vector<std::string> m_sceneList;
		bool m_sceneListDirty = true;
		// Set by RequestSaveAsPopup/RequestOpenPopup; consumed at the top of the
		// next OnImGui to open the popup below from this window's ID scope.
		bool m_requestSaveAsPopup = false;
		bool m_requestOpenPopup = false;
		// Integrated asset lists.
		bool m_assetListsDirty = true;
		std::vector<std::string> m_modelList;
		std::vector<std::string> m_prefabList;
		std::vector<std::string> m_materialList;
		std::vector<std::string> m_textureList;
		std::vector<std::string> m_scriptList;
		char m_assetSearch[64] = {};
		char m_newScriptNameBuf[64] = {};
		std::string m_newScriptError;
		// Save-as-prefab popup (opened from the row context menu; the popup is
		// begun at window level after the tree walk).
		Entity m_prefabSaveTarget{};
		char m_prefabNameBuf[48] = "";
		bool m_openPrefabSave = false;
		// Kind filter chips (OR-combined; none active == show everything).
		bool m_filterMesh = false;
		bool m_filterSkinned = false;
		bool m_filterPhysics = false;
		bool m_filterEffect = false;

		// Inline rename state.
		Entity m_renaming{};
		char m_renameBuf[64] = {};
		bool m_renameFocusPending = false;

		// Shift-range selection works over the rows as the user sees them; the
		// previous frame's order is complete by the time a click arrives.
		std::vector<Entity> m_rowsCur;
		std::vector<Entity> m_rowsPrev;
		std::vector<Entity> m_filteredRowsScratch;
		std::vector<FlatTreeEntry> m_flatTree;
		std::unordered_set<std::uint32_t> m_expandedNodes;
		Entity m_rangeAnchor{};
		// Plain-press on a multi-selected row defers the collapse to release (the
		// press may start a multi-entity drag); this remembers where it landed.
		Entity m_pendingCollapse{};
		// Plain row selection is also deferred until release so starting a drag
		// from the hierarchy does not steal inspector focus before the drop.
		Entity m_pendingClick{};
		bool m_pendingClickCtrl = false;
		bool m_pendingClickShift = false;

		// Tri-zone drag-drop reparent (queued during the tree walk, applied after):
		// SetParent mutates children vectors the recursion may still be iterating.
		std::optional<PendingReparent> m_pendingReparent;
		// Duplicate (context menu / Ctrl+D) also defers past the walk: it
		// creates entities, which would invalidate the iteration. The
		// clipboard trio defers the same way for the context menu items.
		bool m_pendingDuplicate = false;
		bool m_pendingCopy = false;
		bool m_pendingCut = false;
		bool m_pendingPaste = false;

		// Juice: newly-seen entities flash briefly; selection changes pulse.
		std::unordered_set<std::uint32_t> m_knownIds;
		std::unordered_set<std::uint32_t> m_knownIdsScratch;
		std::unordered_map<std::uint32_t, double> m_spawnFlash; // id -> first-seen time
		bool m_knownSeeded = false;                             // no flash on the initial population
		std::uint64_t m_seenSelectionSerial = 0;
		double m_pulseStart = -1.0;

		// Dirty state (unsaved changes indicator).
		bool m_dirty = false;

		// Keyboard arrow navigation: the row index in m_rowsCur that has
		// keyboard focus. -1 means "no focus, click or keyboard first".
		int m_focusedRowIndex = -1;
		KeyboardFocusScope m_keyboardFocusScope = KeyboardFocusScope::None;

		// Type-to-jump: accumulate typed characters with a timeout.
		std::string m_typeJumpText;
		double m_typeJumpTime = -1.0;

		// Entity to scroll into view on the next frame (set by F key).
		Entity m_scrollToEntity{};

		// Group / Ungroup pending flags (applied after the tree walk).
		bool m_pendingGroup = false;
		bool m_pendingUngroup = false;

		// Persistent expansion state: maps hierarchy path -> expanded.
		// Populated from LoadSettings; used to seed m_expandedNodes.
		std::unordered_map<std::string, bool> m_expandedPaths;
		bool m_expandedPathsLoaded = false;
	};
} // namespace aether::app
