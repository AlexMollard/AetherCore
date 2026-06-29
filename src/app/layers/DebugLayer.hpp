#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "AppLayer.hpp"
#include "utils/TomlConfig.hpp"

#include "debug/DebugPanel.hpp"

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

	private:
		void PollScriptErrors(LayerContext& context);
		void LoadSettings(LayerContext& context);
		void SaveSettings(LayerContext& context);
		void PersistSettings(LayerContext& context);

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);
		static void OpenInVSCode(const std::string& filePath, int line);

		bool m_visible = true;

		TomlConfig m_debugConfig;
		std::deque<ScriptErrorToast> m_errorToasts;
		bool m_dockspaceBuilt = false;

		std::vector<std::unique_ptr<DebugPanel>> m_panels;
	};
} // namespace aether::app
