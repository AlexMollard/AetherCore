#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
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

		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		int m_port = 8787;
		bool m_autoStart = false;
		bool m_didAutoStart = false;
	};
} // namespace aether::editor
