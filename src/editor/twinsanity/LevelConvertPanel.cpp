#include "twinsanity/LevelConvertPanel.hpp"

#include "layers/AppLayer.hpp"
#include "PlayState.hpp"
#include "twinsanity/LevelConvert.hpp"
#include "utils/ServiceContainer.hpp"

#include "imgui.h"

namespace aether::editor::twinsanity
{
	void LevelConvertPanel::OnImGui(app::LayerContext& context)
	{
		if (ImGui::Begin(GetName().data(), VisiblePtr()))
		{
			auto* playState = context.TryGet<app::PlayState>();
			const bool playing = playState != nullptr && (playState->IsPlaying() || playState->IsCompiling());
			ImGui::TextDisabled("%s", m_report.c_str());
			ImGui::TextDisabled("Edited the convert script? Press Play then Stop once first:\nthe command runs the last-loaded script assembly.");
			ImGui::Separator();
			if (playing)
			{
				ImGui::TextDisabled("Stop the play session to convert.");
				ImGui::End();
				return;
			}
			LevelConvertRequest request;
			bool run = false;
			if (ImGui::Button("Report"))
			{
				request.report = true;
				run = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Write"))
			{
				run = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Overwrite all"))
			{
				request.overwrite = {"*"};
				run = true;
			}
			if (run)
			{
				const nlohmann::json result = ConvertLevel(context.services, request);
				m_report = result.value("ok", false) ? result.dump(2) : "Convert failed: " + result.value("error", std::string{});
			}
		}
		ImGui::End();
	}
} // namespace aether::editor::twinsanity
