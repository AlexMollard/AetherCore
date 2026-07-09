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
#include "debug/SceneSelection.hpp"
#include "debug/ScriptErrorOverlay.hpp"
#include "debug/UndoStack.hpp"

namespace aether::app
{
	class HierarchyPanel;

	class DebugLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void OnRenderTargetsInvalidated(LayerContext& context) override;

	private:
		// Bottom-of-viewport status bar (scene, play state, resolution, FPS). Only
		// drawn from the second frame on, so it never resizes the docked viewport
		// before its render targets exist.
		void DrawStatusBar(LayerContext& context);
		// Ctrl+P fuzzy command palette (panel toggles, play, layout).
		void DrawCommandPalette(LayerContext& context);
		// Named layout presets: capture/apply the ImGui dock ini + panel visibility.
		void ReloadLayoutPresets();
		void ApplyLayoutPreset(const LayoutPreset& preset);
		void CaptureCurrentLayout(std::string name);
		void DeleteLayoutPreset(std::string_view name);
		[[nodiscard]] DebugPanel* FindPanelByName(std::string_view name) const;
		void LoadSettings(LayerContext& context);
		void SaveSettings(LayerContext& context);
		void PersistSettings(LayerContext& context);
		// File > Save and Ctrl+S: quick-saves to the current scene name (tracked
		// by SceneSubsystem), falling back to the Scene Outliner's Save-As popup
		// when there isn't one yet (or the quick-save failed).
		void SaveCurrentScene(LayerContext& context);

		SceneSelection m_selection;
		UndoStack m_undoStack;
		// Selection-outline pulse bookkeeping (brightness eases after changes).
		std::uint64_t m_outlineSeenSerial = 0;
		double m_outlinePulseStart = -1.0;
		TomlConfig m_debugConfig;
		ScriptErrorOverlay m_scriptErrors;
		bool m_dockspaceBuilt = false;
		// Set by the Window > Reset Layout menu item; forces the default dock layout
		// to be rebuilt on the next frame.
		bool m_resetLayout = false;

		// Command palette (Ctrl+P) state.
		char m_paletteQuery[128] = {};
		int m_paletteSelected = 0;

		// Named layout presets (persisted under the user config dir) and the
		// deferred-apply state: a preset's ini is loaded at the top of the next
		// frame, before any window Begin(), so docking settings take effect cleanly.
		std::vector<LayoutPreset> m_layoutPresets;
		bool m_pendingLayoutApply = false;
		std::string m_pendingLayoutIni;
		std::vector<std::pair<std::string, bool>> m_pendingLayoutVisibility;
		bool m_openSavePresetPopup = false;
		char m_newPresetName[64] = {};

		EditorProjectManager m_projects;

		std::vector<std::unique_ptr<DebugPanel>> m_panels;
		// Non-owning: observes the HierarchyPanel instance owned by m_panels, so
		// the File menu / Ctrl+S can drive its Save-As / Open popups instead of
		// duplicating them. Set in OnAttach, cleared in OnDetach.
		HierarchyPanel* m_hierarchyPanel = nullptr;
	};
} // namespace aether::app
