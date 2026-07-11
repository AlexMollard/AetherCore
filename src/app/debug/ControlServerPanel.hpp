#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	// Editor window for managing the ControlServer - the localhost endpoint that
	// the AetherCore MCP and the aether-ctl CLI drive. Start/stop the server,
	// choose the port, toggle auto-start, and watch live request stats. Starts
	// hidden (niche developer tool; enable via Window menu).
	class ControlServerPanel final : public DebugPanel
	{
	public:
		[[nodiscard]] std::string_view GetName() const override
		{
			return "Control Server";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		int m_port = 8787;
		bool m_autoStart = false;
		bool m_didAutoStart = false; // one-shot guard for the deferred auto-start
	};
} // namespace aether::app
