#pragma once

#include <deque>
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
#include "debug/SceneSelection.hpp"
#include "debug/UndoStack.hpp"

namespace aether::app
{
	// Non-blocking error notification for script errors.
	// Always visible regardless of m_visible (the debug panel toggle).
	struct ScriptErrorToast
	{
		std::string message;
		std::string summary;
		std::string filePath;
		int line = 0;
		bool dismissed = false;
	};

	class DebugLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void OnRenderTargetsInvalidated(LayerContext& context) override;

	private:
		void PollScriptErrors(LayerContext& context);
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

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);
		static void OpenInVSCode(const std::string& filePath, int line);

		SceneSelection m_selection;
		UndoStack m_undoStack;
		// Selection-outline pulse bookkeeping (brightness eases after changes).
		std::uint64_t m_outlineSeenSerial = 0;
		double m_outlinePulseStart = -1.0;
		TomlConfig m_debugConfig;
		std::deque<ScriptErrorToast> m_errorToasts;
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

		std::vector<std::unique_ptr<DebugPanel>> m_panels;
	};
} // namespace aether::app
