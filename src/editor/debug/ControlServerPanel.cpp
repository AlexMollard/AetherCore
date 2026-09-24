#include "debug/ControlServerPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <ranges>

#include <imgui.h>

#include "Color.hpp"
#include "editor/ControlServer.hpp"
#include "layers/AppLayer.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
		ImVec4 ToImVec4(const glm::vec4& c)
		{
			return {c.r, c.g, c.b, c.a};
		}
	} // namespace

	void ControlServerPanel::OnUpdate(app::LayerContext& context)
	{
		if (m_didAutoStart)
		{
			return;
		}
		m_didAutoStart = true;
		if (!m_autoStart)
		{
			return;
		}
		if (auto* server = context.TryGet<editor::ControlServer>(); server != nullptr && !server->IsRunning())
		{
			server->Start(m_port);
		}
	}

	void ControlServerPanel::OnImGui(app::LayerContext& context)
	{
		if (ImGui::Begin("Control Server", VisiblePtr()))
		{
			chrome::PanelHeader("CONTROL SERVER");
			auto* server = context.TryGet<editor::ControlServer>();
			if (server == nullptr)
			{
				ImGui::TextColored(chrome::kError, "Control server unavailable (editor build only).");
			}
			else
			{
				const bool running = server->IsRunning();
				if (running)
				{
					ImGui::TextColored(chrome::kSuccess, "%s", "\xE2\x97\x8F Listening");
					ImGui::SameLine();
					ImGui::Text("127.0.0.1:%d", server->Port());
				}
				else
				{
					ImGui::TextColored(chrome::kMuted, "%s", "\xE2\x97\x8B Stopped");
				}

				ImGui::Spacing();
				ImGui::BeginDisabled(running);
				if (ImGui::InputInt("Port", &m_port))
				{
					m_port = std::clamp(m_port, 1, 65535);
				}
				ImGui::EndDisabled();

				if (running)
				{
					if (ImGui::Button("Stop"))
					{
						server->Stop();
					}
				}
				else if (ImGui::Button("Start"))
				{
					server->Start(m_port);
				}
				ImGui::SameLine();
				ImGui::Checkbox("Auto-start on launch", &m_autoStart);

				ImGui::SeparatorText("Stats");
				ImGui::Text("Requests handled: %llu", static_cast<unsigned long long>(server->RequestCount()));
				ImGui::Text("Connected clients: %d", server->ConnectedClients());

				const auto log = server->RecentRequests();
				if (!log.empty() && ImGui::BeginTable("recent_requests", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Method");
					ImGui::TableSetupColumn("Result");
					ImGui::TableHeadersRow();
					for (const auto& it: std::views::reverse(log))
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextUnformatted(it.method.c_str());
						ImGui::TableSetColumnIndex(1);
						ImGui::TextColored(it.ok ? chrome::kSuccess : chrome::kError, "%s", it.ok ? "ok" : "error");
					}
					ImGui::EndTable();
				}

				ImGui::SeparatorText("How to connect");
				ImGui::TextWrapped("%s", "Drive the editor from the AetherCore MCP (see tools/mcp), or from a terminal:");
				ImGui::Text("aether-ctl --port %d info", running ? server->Port() : m_port);
			}
		}
		ImGui::End();
	}

	void ControlServerPanel::LoadSettings(TomlConfig& config, app::LayerContext& /*context*/)
	{
		m_port = static_cast<int>(config.GetFloat("controlserver.port", static_cast<float>(m_port)));
		m_port = std::clamp(m_port, 1, 65535);
		m_autoStart = config.GetBool("controlserver.autostart", m_autoStart);
	}

	void ControlServerPanel::SaveSettings(TomlConfig& config, app::LayerContext& /*context*/) const
	{
		config.Set("controlserver.port", static_cast<float>(m_port));
		config.Set("controlserver.autostart", m_autoStart);
	}
} // namespace aether::editor
