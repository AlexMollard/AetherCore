#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "debug/DebugPanel.hpp"
#include "debug/EditorDragDrop.hpp"
#include "gpu/GpuHandles.hpp"
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
		// View preferences outlive the session. How you like to look at your assets is not
		// something to set again every launch.
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		// What the create menu makes. Kept above the declarations that name it.
		enum class NewAssetKind
		{
			Script,
			Material,
			Folder
		};

		struct Entry
		{
			std::filesystem::path path;
			std::string name;
			std::string payloadPath;
			std::uint64_t sizeBytes = 0;
			// Last write time, so a thumbnail can tell that the asset behind it has changed.
			std::int64_t writeTime = 0;
			dragdrop::FileKind kind = dragdrop::FileKind::Unknown;
			bool isDirectory = false;
			std::vector<Entry> children;
		};

		// How the contents of the current folder are shown. A tree of filenames is a poor way
		// to browse textures and materials, which is most of what a project holds, so the grid
		// is the default and the list stays for when names and sizes matter more than looks.
		// How the contents pane is ordered. Folders always come first regardless: they are
		// navigation, not content, and mixing them into a size or date order buries them.
		enum class SortMode
		{
			Name,
			Type,
			Size,
			Modified
		};

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
			// Where this material's sphere lives in the baker's atlas, or -1 for none.
			// Preferred over the albedo map and the swatch, both of which remain the fallback
			// while it bakes and for materials that cannot be rendered.
			int atlasSlot = -1;
			bool bakeAttempted = false;
			bool bakeReady = false;
			// The file's write time when this was built. Editing a material has to re-bake it,
			// or the tile keeps showing the material as it used to look.
			std::int64_t stamp = 0;
			// Textures upload asynchronously, so an asset seen the instant its folder opens is
			// usually not resident yet. That is a REASON TO WAIT, not a failure - baking then
			// gives a default-looking sphere that never corrects itself. Bounded so a texture
			// that genuinely will not load stops being retried.
			int retries = 0;
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
		// The one ordering. The grid, the list and the keyboard's idea of "next" all read
		// this, so they cannot disagree about what order the folder is in.
		[[nodiscard]] std::vector<const Entry*> SortedChildren(const Entry& dir) const;
		void DrawSortMenu();
		// Click-off to clear, and drag a band over the tiles to select them. Runs after the
		// tiles are drawn, when their rectangles are known.
		void HandleContentsSelectionGestures(app::LayerContext& context);
		// Delete / F2 / Enter / arrows over the contents pane. Everything here already had a
		// menu item; none of it had a key, so the browser could only be driven by mouse.
		void HandleContentsShortcuts(app::LayerContext& context);
		[[nodiscard]] const Entry* FindEntryByPath(const std::string& path) const;
		void SelectOnly(app::LayerContext& context, const Entry& entry);
		void DrawFileTile(app::LayerContext& context, const Entry& entry, float tileSize);
		// Selecting, opening, dragging and the context menu, shared by the list row and the
		// grid tile so the two views cannot drift apart in what an asset does.
		void ApplyEntryInteractions(app::LayerContext& context, const Entry& entry);
		[[nodiscard]] Entry* FindDirectory(Entry& node, const std::filesystem::path& dir);
		void OpenDirectory(app::LayerContext& context, const std::filesystem::path& dir);
		// The ImGui texture for an asset's thumbnail, or 0 when it has none (or when this
		// frame's load budget is spent, in which case the icon is drawn and it loads later).
		[[nodiscard]] const Thumbnail* ThumbnailFor(app::LayerContext& context, const Entry& entry);
		// The baked mesh a model tile can be rendered from, or empty. A raw glTF is only
		// usable once it has been baked, and opening a folder must not trigger that.
		[[nodiscard]] std::string BakedMeshFor(const Entry& entry) const;
		void ReleaseThumbnails(app::LayerContext& context);
		// Drop what was built for an asset while KEEPING its atlas slot, so re-baking an
		// edited material reuses its square instead of consuming a new one every save.
		void InvalidateThumbnail(app::LayerContext& context, Thumbnail& thumb);
		// Bakes at most one material sphere per frame, on a preview instance of its own so
		// that baking never disturbs what the Material window is showing.
		void PumpMaterialThumbnailBakes(app::LayerContext& context);
		void DrawFileRow(app::LayerContext& context, const Entry& entry);
		bool DrawActiveRename(const Entry& entry);
		void DrawSearchResults(app::LayerContext& context, const Entry& entry);
		bool DrawRowContextMenu(app::LayerContext& context, const Entry& entry);
		void DrawPendingPopups(app::LayerContext& context);
		// The one place assets are created. Menu items call this; there is no second path.
		void DrawCreateMenuItems(const std::filesystem::path& dir);
		bool CreateAsset(NewAssetKind kind, const std::filesystem::path& dir);

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
		// Move an asset into another folder, repointing everything that referenced it. This is
		// the same problem rename solves, so it shares rename's retargeting rather than
		// growing a second answer to it.
		bool MoveEntry(const std::filesystem::path& source, const std::filesystem::path& destDir);
		// A folder row or tile as a drop target. Returns true when something was moved.
		bool AcceptFileDropIntoFolder(app::LayerContext& context, const std::filesystem::path& destDir);
		// The physical path behind a "project://..." payload, or empty when it is outside.
		[[nodiscard]] std::filesystem::path PhysicalPathFor(std::string_view vfsPath) const;
		[[nodiscard]] bool IsSelected(const std::string& path) const;
		// Apply a click to the selection: plain replaces, ctrl toggles, shift extends from the
		// primary through the folder's draw order.
		void ClickSelect(const Entry& entry, bool ctrl, bool shift);
		// The paths of the current folder's entries, in the order they are drawn.
		[[nodiscard]] std::vector<std::string> VisibleOrder() const;
		bool DeleteEntry(const std::filesystem::path& target, bool isDirectory);
		// Fill m_deleteTargets from `entry` plus the selection it belongs to, and count what
		// still references them.
		void BeginDelete(const Entry& entry);

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
		// Rubber-band selection. Anchor is where the drag started, in screen space; the tiles
		// drawn this frame are collected so the band can test what it covers.
		bool m_marqueeActive = false;
		ImVec2 m_marqueeAnchor{};
		std::vector<std::pair<std::string, ImRect>> m_frameTiles;

		SortMode m_sortMode = SortMode::Name;
		bool m_sortDescending = false;
		// Columns the grid last drew with, so Up/Down move a row rather than an item.
		int m_gridColumns = 1;
		std::unordered_map<std::string, Thumbnail> m_thumbnails;
		// Opening a folder of hundreds of textures must not stall the frame it is opened on,
		// so only a few thumbnails are decoded per frame and the rest arrive over the next few.
		int m_thumbnailLoadsThisFrame = 0;
		// The material currently on the baker, and the frame its render was requested. The
		// result is only readable once that frame has been through the graph.
		std::string m_bakeInFlight;
		int m_bakeStartedFrame = 0;
		int m_nextAtlasSlot = 0;
		// The baker's atlas, registered with ImGui once and shared by every tile.
		std::uint64_t m_atlasImGuiId = 0;
		int m_atlasColumns = 1;

		char m_search[96] = {};
		std::string m_selectedPath;
		// Every selected asset, in the order they are drawn. m_selectedPath stays the primary -
		// the one the Material and Texture windows follow - and this is what a bulk operation
		// acts on. Cleared whenever the folder changes, since a selection you cannot see is a
		// selection you will delete by accident.
		std::vector<std::string> m_selectedPaths;
		std::string m_selectedPayloadPath;
		dragdrop::FileKind m_selectedKind = dragdrop::FileKind::Unknown;
		std::string m_lastAdoptedAsset;
		bool m_selectedIsDirectory = false;

		std::filesystem::path m_renameTarget;
		char m_renameBuf[128] = {};
		bool m_renameFocusPending = false;

		// Everything the pending delete will remove. A multi-selection deletes together, the
		// way it already moves together - deleting only the row you pressed Delete on, while
		// several are highlighted, is the kind of surprise you cannot undo here.
		struct DeleteTarget
		{
			std::filesystem::path path;
			bool isDirectory = false;
		};

		std::vector<DeleteTarget> m_deleteTargets;
		bool m_openDeletePopup = false;
		// Counted once when the dialog opens, not per frame: it walks every project file.
		int m_deleteReferenceCount = 0;

		// What "New" makes. Picking the KIND first and then naming it, rather than three name
		// fields where the field you typed into decided the kind - which meant reading all
		// three rows to find out what the dialog even did.
		bool m_openNewPopup = false;
		// The asset just created, waiting for the tree to be rescanned so its row exists and
		// can be put straight into rename. Naming happens in place, where the file is.
		std::filesystem::path m_renameAfterScan;
		std::filesystem::path m_createDir;
		std::string m_opError;
	};
} // namespace aether::editor
