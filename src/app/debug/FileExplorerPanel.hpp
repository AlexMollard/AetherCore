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

		void RefreshRoot(app::LayerContext& context);
		void RescanTree();
		void ScanDirectory(const std::filesystem::path& dir, Entry& out, int depth);

		void DrawToolbar(app::LayerContext& context);
		void DrawTree(app::LayerContext& context);
		void DrawDirectoryNode(app::LayerContext& context, Entry& entry, int depth);
		void DrawFileRow(app::LayerContext& context, const Entry& entry);
		bool DrawActiveRename(const Entry& entry);
		void DrawSearchResults(app::LayerContext& context, const Entry& entry);
		bool DrawRowContextMenu(app::LayerContext& context, const Entry& entry);
		void DrawPendingPopups(app::LayerContext& context);

		void UpdatePreview(app::LayerContext& context);
		void ReleasePreview(app::LayerContext& context);
		void DrawPreviewCard(app::LayerContext& context);

		void BeginRename(const Entry& entry);
		bool ApplyRename(const std::filesystem::path& target, std::string_view newName);
		// Point every scene / prefab / material that referenced a renamed asset at its new
		// path. Returns how many files were rewritten.
		int RetargetAssetReferences(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
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

		char m_search[96] = {};
		std::string m_selectedPath;
		std::string m_selectedPayloadPath;
		dragdrop::FileKind m_selectedKind = dragdrop::FileKind::Unknown;
		std::string m_lastAdoptedAsset;
		bool m_selectedIsDirectory = false;

		std::string m_previewLoadedFor = "<none>";
		TextureHandle m_previewTexture{};
		std::uint64_t m_previewImGuiId = 0;
		gpu::Extent2D m_previewExtent;
		std::string m_previewText;
		bool m_previewIsImage = false;
		bool m_previewIsModel = false;
		bool m_previewIsText = false;
		bool m_previewFailed = false;

		std::filesystem::path m_renameTarget;
		char m_renameBuf[128] = {};
		bool m_renameFocusPending = false;

		std::filesystem::path m_deleteTarget;
		bool m_deleteIsDirectory = false;
		bool m_openDeletePopup = false;

		bool m_openNewPopup = false;
		char m_newScriptNameBuf[64] = {};
		char m_newFolderNameBuf[64] = {};
		char m_newMaterialNameBuf[64] = {};
		std::filesystem::path m_createDir;
		std::string m_opError;
	};
} // namespace aether::editor
