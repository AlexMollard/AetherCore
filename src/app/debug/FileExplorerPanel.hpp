#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
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

		void OnAttach(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;

	private:
		void DrawDirectory(LayerContext& context, const std::filesystem::path& dir, int depth);
		void DrawFile(LayerContext& context, const std::filesystem::path& path);
		void RefreshRoot(LayerContext& context);

		std::filesystem::path m_root;
		std::filesystem::path m_scriptRoot;
		std::string m_projectName;
		char m_newScriptNameBuf[64] = {};
		std::string m_newScriptError;
		bool m_rootAvailable = false;
	};
} // namespace aether::app
