#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AppLayer.hpp"
#include "utils/LayoutPresetStore.hpp"
#include "utils/TomlConfig.hpp"

#include "debug/DebugPanel.hpp"
#include "debug/EditorProjectManager.hpp"
#include "debug/EditorWindowActions.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/ScriptErrorOverlay.hpp"
#include "debug/TilePaintingState.hpp"
#include "debug/UndoStack.hpp"
#include "scene/BackgroundSceneWriter.hpp"

namespace aether
{
	class Window;
}

namespace aether::editor
{
	class HierarchyPanel;

	class DebugLayer final : public app::AppLayer
	{
	public:
		void OnAttach(app::LayerContext& context) override;
		void OnDetach(app::LayerContext& context) override;
		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void OnRenderTargetsInvalidated(app::LayerContext& context) override;

	private:
		// drawn from the second frame on, so it never resizes the docked viewport
		void DrawStatusBar(app::LayerContext& context);
		// Ctrl+P fuzzy command palette (panel toggles, play, layout).
		void DrawCommandPalette(app::LayerContext& context);
		// Named layout presets: capture/apply the ImGui dock ini + panel visibility.
		void ReloadLayoutPresets();
		void ApplyLayoutPreset(const LayoutPreset& preset);
		void CaptureCurrentLayout(std::string name);
		void DeleteLayoutPreset(std::string_view name);
		[[nodiscard]] DebugPanel* FindPanelByName(std::string_view name) const;
		void LoadSettings(app::LayerContext& context);
		void SaveSettings(app::LayerContext& context);
		void PersistSettings(app::LayerContext& context);
		bool SaveCurrentScene(app::LayerContext& context);
		void SaveAndReturnToLauncher(app::LayerContext& context);

		void ShowToast(std::string text, bool isError = false);
		void DrawToasts();
		std::string m_toastText;
		double m_toastStart = -1.0;
		bool m_toastError = false;

		void CaptureEditorWindowSize(app::LayerContext& context);
		int m_editorWindowW = 0;
		int m_editorWindowH = 0;

		SceneSelection m_selection;
		UndoStack m_undoStack;
		TilePaintingState m_tilePainting;
		app::scene::BackgroundSceneWriter m_sceneWriter;
		// registers a reference to this, so it must outlive the registration).
		EditorWindowActions m_windowActions;
		std::uint64_t m_outlineSeenSerial = 0;
		double m_outlinePulseStart = -1.0;
		TomlConfig m_debugConfig;
		ScriptErrorOverlay m_scriptErrors;
		bool m_dockspaceBuilt = false;
		// Set by the Window > Reset Layout menu item; forces the default dock layout
		bool m_resetLayout = false;

		char m_paletteQuery[128] = {};
		int m_paletteSelected = 0;

		// Named layout presets (persisted under the user config dir) and the
		std::vector<LayoutPreset> m_layoutPresets;
		bool m_pendingLayoutApply = false;
		std::string m_pendingLayoutIni;
		std::vector<std::pair<std::string, bool>> m_pendingLayoutVisibility;
		bool m_openSavePresetPopup = false;
		char m_newPresetName[64] = {};

		EditorProjectManager m_projects;

		std::vector<std::unique_ptr<DebugPanel>> m_panels;
		HierarchyPanel* m_hierarchyPanel = nullptr;
	};
} // namespace aether::editor
