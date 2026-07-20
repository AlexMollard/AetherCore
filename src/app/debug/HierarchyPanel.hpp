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
	class HierarchyPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Scene Outliner";
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

		void DrawNode(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, int depth, int flatTreeIndex, bool searching, std::string_view needle);
		void FlattenNode(World& world, Entity e, int depth, std::uint64_t openMask);
		void DrawRowBackdrop(const SceneSelection& selection, Entity e, int rowIndex);
		void DrawRowContent(World& world, Entity e, bool searching, std::string_view needle, bool continuePreviousItem = true);
		void HandleRowClick(SceneSelection& selection, Entity e);
		void HandleRowDragDrop(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, float dropMinY, float dropMaxY, float visualMaxX);
		// Returns true if the menu destroyed `e` (callers must not touch it after).
		bool DrawRowContextMenu(World& world, SceneSelection& selection, Entity e);
		void BeginRename(const World& world, Entity e);
		void DrawRowUtilityToggles(World& world, Entity e);
		std::string ComputeEntityPath(const World& world, Entity e) const;
		void SyncExpandedFromPaths(World& world);
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

		bool m_pendingGroup = false;
		bool m_pendingUngroup = false;

		std::unordered_map<std::string, bool> m_expandedPaths;
		bool m_expandedPathsLoaded = false;
	};
} // namespace aether::editor
