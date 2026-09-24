#pragma once

#include <filesystem>
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
#include "SceneSelection.hpp"
#include "debug/ScriptErrorOverlay.hpp"
#include "debug/PixelArtDocument.hpp"
#include "debug/TilePaintingState.hpp"
#include "editor/UndoStack.hpp"
#include "editor/AutosaveService.hpp"
#include "scene/BackgroundSceneWriter.hpp"

namespace aether
{
	class Window;
}

namespace aether::editor
{
	class ViewportPanel;
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
		// Make a panel visible and bring it forward, by the name it reports from GetName().
		// Used by menu items that are about doing a thing rather than toggling a window -
		// File > Publish should open the Build panel, not ask you to find it.
		void ShowPanel(std::string_view name);

		// drawn from the second frame on, so it never resizes the docked viewport
		void DrawStatusBar(app::LayerContext& context);
		// Ctrl+P fuzzy command palette (panel toggles, play, layout).
		void DrawCommandPalette(app::LayerContext& context);
		void DrawShortcutsReference();
		// Named layout presets: capture/apply the ImGui dock ini + panel visibility.
		void ReloadLayoutPresets();
		void ApplyLayoutPreset(const LayoutPreset& preset);
		void CaptureCurrentLayout(std::string name);
		void DeleteLayoutPreset(std::string_view name);
		// Apply a built-in workflow layout (index into the workflow table): rebuilds the
		// dockspace and shows the panels relevant to that workflow.
		void ApplyWorkflowLayout(int index);
		[[nodiscard]] DebugPanel* FindPanelByName(std::string_view name) const;
		void LoadSettings(app::LayerContext& context);
		void SaveSettings(app::LayerContext& context);
		void PersistSettings(app::LayerContext& context);
		bool SaveCurrentScene(app::LayerContext& context);

		// Act on scene writes that finished on the writer thread: drop the recovery copy
		// once the file is really on disk, and put the scene back to unsaved with a visible
		// error if it is not.
		void DrainSceneWrites(app::LayerContext& context);
		void SaveAndReturnToLauncher(app::LayerContext& context);

		// Undo or redo one step, including the selection remap both need. Shared by the
		// Ctrl+Z/Y shortcut and the Edit menu so the two cannot drift apart.
		void ApplyHistoryStep(app::LayerContext& context, bool redo);

		// Rebuilds the flavor panel set and theme for the given project (invoked by the
		// project manager on every open, so a flavor switch between projects is clean).
		void ApplyProjectFlavor(const app::EditorProjectContext& project);

		// Actions that throw away the scene in memory. Each is routed through
		// ConfirmDiscard so it cannot run over unsaved work without being asked.
		enum class PendingNav
		{
			None,
			NewScene3D,
			NewScene2D,
			OpenScene,
			CloseEditor,
		};
		// True when the scene in memory differs from the file on disk. Covers tilemap
		// cells too: those live in their own .tiles asset, so a scene can be "clean" by
		// history and still have unsaved paint.
		[[nodiscard]] bool HasUnsavedWork() const;

		// Names of the panels holding an edited document that is not on disk.
		[[nodiscard]] std::vector<std::string_view> UnsavedDocuments(app::LayerContext& context) const;
		// Run nav now if there is nothing to lose, otherwise raise the prompt and run it
		// once the user has answered.
		void ConfirmDiscard(app::LayerContext& context, PendingNav nav);
		void RunPendingNav(app::LayerContext& context);
		void DrawUnsavedChangesPopup(app::LayerContext& context);
		// Intercepts the OS close request so the prompt gets a chance to appear.
		void PollCloseRequest(app::LayerContext& context);
		PendingNav m_pendingNav = PendingNav::None;
		bool m_openUnsavedPopup = false;

		// Autosave leaves a recovery copy beside the project when the editor dies with
		// unsaved work. Restoring one is deliberately never automatic, so until now the
		// copies existed and nothing in the editor ever mentioned them - the offer is made
		// here, once, when a project opens.
		void PollRecoveryOffer(app::LayerContext& context);
		void DrawRecoveryPopup(app::LayerContext& context);
		// Project whose recovery copies have already been looked for, so the scan (and the
		// offer) happen once per open rather than every frame.
		std::filesystem::path m_recoveryCheckedRoot;
		std::vector<RecoveredScene> m_recoverable;
		bool m_openRecoveryPopup = false;
		std::string m_recoveryError;

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
		PixelArtDocument m_pixelArt;
		app::scene::BackgroundSceneWriter m_sceneWriter;
		// registers a reference to this, so it must outlive the registration).
		EditorWindowActions m_windowActions;
		std::uint64_t m_outlineSeenSerial = 0;
		double m_outlinePulseStart = -1.0;
		TomlConfig m_debugConfig;
		ScriptErrorOverlay m_scriptErrors;
		// Periodic crash insurance; writes a recovery copy, never the scene file itself.
		editor::AutosaveService m_autosave;
		bool m_dockspaceBuilt = false;
		// Selected built-in workflow layout to apply on the next dock rebuild (-1 = the
		// default layout). Consumed alongside m_resetLayout.
		int m_pendingWorkflowLayout = -1;
		// Set by the Window > Reset Layout menu item; forces the default dock layout
		bool m_resetLayout = false;

		// Set when a layout is rebuilt, spent one frame later to select the Viewport tab in
		// the centre dock node (see the note at the panel draw loop).
		bool m_focusViewportAfterLayout = false;

		// Raised by the Edit menu; the palette also opens itself on Ctrl+P.
		bool m_openCommandPalette = false;
		bool m_openShortcuts = false;
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

		// Borrowed from the layer stack for the lifetime of the layer; the flavor handler
		// needs it when a project opens mid-frame.
		app::LayerContext* m_layerContext = nullptr;
		// Panels added for the open project's flavor (raw pointers into m_panels). Rebuilt
		// on every project open so switching projects swaps flavors cleanly.
		std::vector<DebugPanel*> m_flavorPanels;

		std::vector<std::unique_ptr<DebugPanel>> m_panels;
		// Deferred so the focus lands inside the ImGui frame; the request may arrive from a
		// control command drained before one has begun.
		std::string m_pendingFocusWindow;
		// Undo/redo steps requested through the control endpoint, drained next frame
		// alongside the keyboard shortcut.
		int m_pendingUndoSteps = 0;
		int m_pendingRedoSteps = 0;
		// Open-scene dialog asked for through the control endpoint, routed through the same
		// unsaved-work check the File menu uses.
		bool m_pendingOpenSceneDialog = false;
		// Borrowed from m_panels, which owns it. Held typed so the edit camera can be
		// written once on shutdown rather than from the per-frame settings path.
		ViewportPanel* m_viewportPanel = nullptr;
		HierarchyPanel* m_hierarchyPanel = nullptr;
	};
} // namespace aether::editor
