#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "debug/EditorDragDrop.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/TextureHandle.hpp"

namespace aether::editor
{
	// file operations, and on a staleness timer) - never walked per frame. Rows
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
		struct Entry
		{
			std::filesystem::path path;
			std::string name;
			std::string payloadPath;
			std::uint64_t sizeBytes = 0;
			dragdrop::FileKind kind = dragdrop::FileKind::Unknown;
			bool isDirectory = false;
			std::vector<Entry> children;
		};

		// How the contents of the current folder are shown. A tree of filenames is a poor way
		// to browse textures and materials, which is most of what a project holds, so the grid
		// is the default and the list stays for when names and sizes matter more than looks.
		enum class ViewMode
		{
			Grid,
			List
		};

		// One cached thumbnail. Resolving a texture to an ImGui id walks every registered
		// texture, so this is done once per asset and kept, not repeated per frame.
		struct Thumbnail
		{
			TextureHandle texture{};
			std::uint64_t imguiId = 0;
			// A material with no albedo map still has a colour, which says far more about it
			// than a generic icon does.
			std::uint32_t swatch = 0;
			bool hasSwatch = false;
			bool resolved = false;
		};

		void RefreshRoot(app::LayerContext& context);
		void RescanTree();
		void ScanDirectory(const std::filesystem::path& dir, Entry& out, int depth);

		void DrawToolbar(app::LayerContext& context);
		// Left pane: folders only. Files live in the contents pane, so the tree stays a map of
		// the project rather than a wall of every file in it.
		void DrawFolderTree(app::LayerContext& context, Entry& entry);
		void DrawBreadcrumb(app::LayerContext& context);
		void DrawFolderContents(app::LayerContext& context);
		void DrawFileTile(app::LayerContext& context, const Entry& entry, float tileSize);
		// Selecting, opening, dragging and the context menu, shared by the list row and the
		// grid tile so the two views cannot drift apart in what an asset does.
		void ApplyEntryInteractions(app::LayerContext& context, const Entry& entry);
		[[nodiscard]] Entry* FindDirectory(Entry& node, const std::filesystem::path& dir);
		void OpenDirectory(app::LayerContext& context, const std::filesystem::path& dir);
		// The ImGui texture for an asset's thumbnail, or 0 when it has none (or when this
		// frame's load budget is spent, in which case the icon is drawn and it loads later).
		[[nodiscard]] const Thumbnail* ThumbnailFor(app::LayerContext& context, const Entry& entry);
		void ReleaseThumbnails(app::LayerContext& context);
		void DrawFileRow(app::LayerContext& context, const Entry& entry);
		bool DrawActiveRename(const Entry& entry);
		void DrawSearchResults(app::LayerContext& context, const Entry& entry);
		bool DrawRowContextMenu(app::LayerContext& context, const Entry& entry);
		void DrawPendingPopups(app::LayerContext& context);

		void BeginRename(const Entry& entry);
		bool ApplyRename(const std::filesystem::path& target, std::string_view newName);
		// Point every scene / prefab / material that referenced a renamed asset at its new
		// path. Returns how many files were rewritten.
		int RetargetAssetReferences(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);

		// "project://..." for a path inside the project, or empty for one outside it.
		[[nodiscard]] std::string VfsPathFor(const std::filesystem::path& path) const;
		// How many project files reference the asset (or, for a directory, anything inside
		// it). Deleting cannot repoint those references, so the count is what the confirm
		// dialog warns with.
		[[nodiscard]] int CountAssetReferences(const std::filesystem::path& target, bool isDirectory) const;
		bool DuplicateEntry(const std::filesystem::path& target);
		bool DeleteEntry(const std::filesystem::path& target, bool isDirectory);

		std::filesystem::path m_root;
		std::filesystem::path m_scriptRoot;
		std::string m_projectName;
		bool m_rootAvailable = false;

		Entry m_tree;
		bool m_treeDirty = true;
		double m_lastScanTime = 0.0;
		int m_fileCount = 0;
		int m_dirCount = 0;
		std::string m_scanError;

		std::filesystem::path m_currentDir;
		// Set by a tile and applied after the grid is drawn: opening a folder drops the
		// thumbnail cache that the remaining tiles are still reading from.
		std::filesystem::path m_pendingOpenDir;
		ViewMode m_viewMode = ViewMode::Grid;
		float m_tileSize = 104.0f;
		std::unordered_map<std::string, Thumbnail> m_thumbnails;
		// Opening a folder of hundreds of textures must not stall the frame it is opened on,
		// so only a few thumbnails are decoded per frame and the rest arrive over the next few.
		int m_thumbnailLoadsThisFrame = 0;

		char m_search[96] = {};
		std::string m_selectedPath;
		std::string m_selectedPayloadPath;
		dragdrop::FileKind m_selectedKind = dragdrop::FileKind::Unknown;
		std::string m_lastAdoptedAsset;
		bool m_selectedIsDirectory = false;

		std::filesystem::path m_renameTarget;
		char m_renameBuf[128] = {};
		bool m_renameFocusPending = false;

		std::filesystem::path m_deleteTarget;
		bool m_deleteIsDirectory = false;
		bool m_openDeletePopup = false;
		// Counted once when the dialog opens, not per frame: it walks every project file.
		int m_deleteReferenceCount = 0;

		bool m_openNewPopup = false;
		char m_newScriptNameBuf[64] = {};
		char m_newFolderNameBuf[64] = {};
		char m_newMaterialNameBuf[64] = {};
		std::filesystem::path m_createDir;
		std::string m_opError;
	};
} // namespace aether::editor
