#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "debug/EditorDragDrop.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/TextureHandle.hpp"

namespace aether::editor
{
	// Project file browser. The directory tree is CACHED (rescanned on demand, on
	// file operations, and on a staleness timer) - never walked per frame. Rows
	// carry per-kind icons, drag-drop payloads, a context menu (open / show in
	// OS / copy path / rename / duplicate / delete / new folder + script), and a
	// flat search mode with project-relative paths.
	class FileExplorerPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "File Explorer";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnAttach(app::LayerContext& context) override;
		void OnDetach(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	private:
		// One cached filesystem entry. Children are only populated for directories.
		struct Entry
		{
			std::filesystem::path path;
			std::string name;        // filename, UTF-8
			std::string payloadPath; // project://relative for assets, absolute for scripts
			std::uint64_t sizeBytes = 0;
			dragdrop::FileKind kind = dragdrop::FileKind::Unknown;
			bool isDirectory = false;
			std::vector<Entry> children;
		};

		void RefreshRoot(app::LayerContext& context);
		void RescanTree();
		void ScanDirectory(const std::filesystem::path& dir, Entry& out, int depth);

		void DrawToolbar(app::LayerContext& context);
		void DrawTree(app::LayerContext& context);
		void DrawDirectoryNode(app::LayerContext& context, Entry& entry, int depth);
		void DrawFileRow(app::LayerContext& context, const Entry& entry);
		void DrawSearchResults(app::LayerContext& context, const Entry& entry);
		bool DrawRowContextMenu(app::LayerContext& context, const Entry& entry); // true = tree mutated
		void DrawPendingPopups(app::LayerContext& context);

		// File operations (all rescan on success and surface errors in the status line).
		// Preview card for the selected file (image thumbnail / text excerpt / meta).
		void UpdatePreview(app::LayerContext& context);
		void ReleasePreview(app::LayerContext& context);
		void DrawPreviewCard(app::LayerContext& context);

		void BeginRename(const Entry& entry);
		bool ApplyRename(const std::filesystem::path& target, std::string_view newName);
		bool DuplicateEntry(const std::filesystem::path& target);
		bool DeleteEntry(const std::filesystem::path& target, bool isDirectory);

		std::filesystem::path m_root;
		std::filesystem::path m_scriptRoot;
		std::string m_projectName;
		bool m_rootAvailable = false;

		Entry m_tree;                // cached root
		bool m_treeDirty = true;     // rescan before next draw
		double m_lastScanTime = 0.0; // staleness timer (seconds, ImGui clock)
		int m_fileCount = 0;
		int m_dirCount = 0;
		std::string m_scanError;

		char m_search[96] = {};
		std::string m_selectedPath;        // generic_string of the selected entry
		std::string m_selectedPayloadPath; // project:// for assets, absolute for scripts
		dragdrop::FileKind m_selectedKind = dragdrop::FileKind::Unknown;
		std::string m_lastAdoptedAsset; // external SceneSelection asset we last mirrored
		bool m_selectedIsDirectory = false;

		// Preview cache, valid while m_previewLoadedFor == m_selectedPath.
		std::string m_previewLoadedFor = "<none>";
		TextureHandle m_previewTexture{};   // registry ref held while an image preview is shown
		std::uint64_t m_previewImGuiId = 0; // registered ImGui texture for the image view
		gpu::Extent2D m_previewExtent{};
		std::string m_previewText; // truncated text excerpt
		bool m_previewIsImage = false;
		bool m_previewIsModel = false;
		bool m_previewIsText = false;
		bool m_previewFailed = false;

		// Inline rename state (armed from the context menu).
		std::filesystem::path m_renameTarget;
		char m_renameBuf[128] = {};
		bool m_renameFocusPending = false;

		// Delete confirmation state.
		std::filesystem::path m_deleteTarget;
		bool m_deleteIsDirectory = false;
		bool m_openDeletePopup = false;

		// "+ New" popover state.
		bool m_openNewPopup = false; // armed by "New here..." in a row context menu
		char m_newScriptNameBuf[64] = {};
		char m_newFolderNameBuf[64] = {};
		std::filesystem::path m_createDir; // where New creates (selected dir or root)
		std::string m_opError;             // last file-operation error, shown in the status line
	};
} // namespace aether::editor
