#pragma once

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vector>

#include "debug/MaterialAssetEditor.hpp"
#include "debug/DebugPanel.hpp"
#include "material/PipelineCache.hpp"
#include "material/PipelineCache.hpp"
#include "materialgraph/MaterialGraph.hpp"
#include "mesh/PrimitiveMeshes.hpp"

namespace aether::editor
{
	// The one window for authoring a material.
	//
	// A material is ONE file. Its properties, its textures and - when it has one - the node
	// graph that generates its shader all live in the same .material.toml and are all edited
	// here at once: the graph reads the material's texture slots, so an editor that showed
	// one or the other meant a graph whose textures could not be assigned.
	class MaterialGraphPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Material";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

		// The graph binds Delete for its own nodes and links, so the scene must not also
		// delete the selected entity on the same keypress.
		[[nodiscard]] bool OwnsEditingKeys() const override
		{
			return m_focused && IsVisible();
		}

		// Ctrl+S saves the scene. With this window focused and a material half-edited, that
		// saved the wrong document and left the edits unsaved - so it gets asked first.
		// Returns true when it handled the save.
		bool SaveIfFocusedAndDirty(app::LayerContext& context);
		// Ctrl+Z / Ctrl+Y route here when this panel has focus, the same way Ctrl+S does:
		// a material is its own document and does not belong in the scene's history.
		bool UndoIfFocused();
		bool RedoIfFocused();

	public:
		[[nodiscard]] bool HasUnsavedWork(app::LayerContext& context) const override;
		bool SaveUnsavedWork(app::LayerContext& context) override;

	private:
		void FollowSelection(app::LayerContext& context);
		void Open(app::LayerContext& context, const std::string& materialPath);

		// A material waiting to be opened because the one on screen has unsaved edits. There
		// is no undo for a file, so switching must not throw work away silently.
		std::string m_pendingOpenPath;
		void DrawUnsavedSwitchPrompt(app::LayerContext& context);

	public:
		void DrawToolbar(app::LayerContext& context);
		void DrawSidebar(app::LayerContext& context, float width);
		void DrawCanvas();
		void DrawAddNodeMenu();
		void SyncLinks();
		void DeleteSelection();
		// Starts a compile if one is not already running. The graph is turned into shader
		// source here, on this thread, because that costs half a millisecond; slangc is left
		// to a worker because it costs five hundred.
		void RequestCompile(app::LayerContext& context);
		// Finishes a compile whose slangc run has completed. Everything here touches the GPU
		// or the asset registry, so it must happen on this thread.
		void PollCompile(app::LayerContext& context);
		bool WriteMaterial(app::LayerContext& context);
		void RefreshPreview(app::LayerContext& context);
		void DrawPreviewControls(app::LayerContext& context, float width);
		void DrawPreview(float side) const;
		[[nodiscard]] std::string GraphSignature() const;

		// Whether the node graph on screen differs from the one in the file. Compared against
		// the saved spec directly rather than tracked with a flag: nothing in the graph editor
		// set a dirty flag at all, so a graph-only edit showed no unsaved marker and the
		// switch guard let it be discarded without asking.
		[[nodiscard]] bool GraphDiffersFromDisk() const;
		// The graph's signature as of the last load or save. NOT the spec's graphSection: the
		// load path re-serialises and re-parses, so the text from the file and the text this
		// graph produces are not byte-identical, and comparing them marks every freshly
		// opened material as unsaved.
		std::string m_savedGraphSignature;

		// The material being edited, and its properties/textures.
		std::string m_path;
		MaterialAssetEditState m_edit;
		// Whether this window had the keyboard last frame; focus can only be read while
		// drawing, and the shortcut is handled elsewhere.
		bool m_focused = false;

		// Present only when this material's shader comes from a graph.
		std::optional<MaterialGraph> m_graph;

		// Whole-graph snapshots, the same model PixelArtDocument uses: a graph is small, and
		// copying one is far simpler than a per-operation command for every edit the node
		// editor can make. Taken BEFORE a change, so the top of m_undoHistory is the state to
		// go back to. Node positions ride along in the snapshot, so an undo also restores the
		// layout as it stood at that moment.
		static constexpr std::size_t kMaxGraphUndo = 64;
		std::vector<MaterialGraph> m_undoHistory;
		std::vector<MaterialGraph> m_redoHistory;

		void SnapshotGraph();
		// History belongs to ONE material. Without clearing it on every path that replaces the
		// graph, undoing after opening a second material restores the FIRST one's graph into
		// it - and auto-compile then writes that over the second material's file.
		void ClearGraphHistory();
		// Snapshot when a widget is first grabbed, so a drag is one undo step and not one per
		// frame. Call immediately after the widget.
		void SnapshotOnActivate();
		void RestoreGraph(std::vector<MaterialGraph>& from, std::vector<MaterialGraph>& to);
		std::string m_compiledSignature; // the graph as last compiled, to detect a real change
		std::string m_submittedSignature; // the graph as last SENT to the compiler
		// The slangc run in flight. Held as a future rather than a raw thread so that
		// destroying the panel joins it: a worker writing into a compile directory after the
		// editor has torn down would be a crash on exit.
		struct PendingCompile
		{
			// The pipelines the worker built from the new SPIR-V, or nullopt if slangc
			// failed. Returned THROUGH the future rather than written into this struct: the
			// struct is moved into m_compiling right after the worker starts, so a pointer to
			// it would dangle.
			std::future<std::optional<std::vector<PipelineCache::PreparedReload>>> result;
			std::string materialPath; // ignored on completion if the selection moved on
			std::string signature;
			std::string shaderVfsPath;
		};
		std::optional<PendingCompile> m_compiling;
		std::string m_status;
		bool m_statusIsError = false;
		bool m_positionsApplied = false;
		bool m_autoCompile = true;
		float m_idleSeconds = 0.0f;

		// The add menu is opened from inside the canvas child but SUBMITTED at the window
		// level: a popup opened in one window and begun in another never appears, which is
		// why right-click did nothing at all.
		bool m_addMenuRequested = false;
		float m_addNodeScreenX = 0.0f;
		float m_addNodeScreenY = 0.0f;
		// Where a node dropped from the toolbar button lands, since there is no cursor to
		// take a position from.
		float m_canvasOriginX = 0.0f;
		float m_canvasOriginY = 0.0f;
		// The add menu filters as you type, because a categorised list of two dozen nodes is
		// still slower than knowing the name.
		char m_addFilter[64] = {};
		bool m_addFilterFocus = false;

		// Preview.
		std::uint64_t m_previewImGuiId = 0;
		std::string m_previewError;
		MaterialPresetSpec m_previewSpec;
		bool m_previewValid = false;
		PrimitiveMesh m_previewMesh = PrimitiveMesh::Sphere;
		// Draggable, because how much room the properties want and how much the canvas wants
		// depends entirely on what you are doing to the material.
		float m_sidebarWidth = 300.0f;
	};
} // namespace aether::editor
