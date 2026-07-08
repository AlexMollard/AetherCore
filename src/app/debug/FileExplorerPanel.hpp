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
			return false;
		}

		void OnAttach(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;

	private:
		void DrawDirectory(const std::filesystem::path& dir, int depth);
		void DrawFile(const std::filesystem::path& path);
		void RefreshRoot();

		std::filesystem::path m_root;
		char m_newScriptNameBuf[64] = {};
		std::string m_newScriptError;
		bool m_rootAvailable = false;
	};
} // namespace aether::app
