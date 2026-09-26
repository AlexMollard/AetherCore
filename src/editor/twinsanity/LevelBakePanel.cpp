#include "twinsanity/LevelBakePanel.hpp"

#include "layers/AppLayer.hpp"
#include "PlayState.hpp"
#include "scene/SceneSubsystem.hpp"
#include "twinsanity/LevelBake.hpp"
#include "utils/ServiceContainer.hpp"

#include <cstdio>

#include "imgui.h"

namespace aether::editor::twinsanity
{
	void LevelBakePanel::OnImGui(app::LayerContext& context)
	{
		if (ImGui::Begin(GetName().data(), VisiblePtr()))
		{
			auto* scenes = context.TryGet<SceneSubsystem>();
			auto* playState = context.TryGet<app::PlayState>();
			const bool playing = playState != nullptr && (playState->IsPlaying() || playState->IsCompiling());
			const char* scene = scenes != nullptr ? scenes->GetCurrentScene().c_str() : "";
			ImGui::Text("Scene: %s", scene != nullptr && *scene != '\0' ? scene : "(none)");
			ImGui::TextDisabled("%s", m_report.c_str());
			ImGui::Separator();
			if (playing)
			{
				ImGui::TextDisabled("Stop the play session to bake.");
			}
			else if (m_running)
			{
				ImGui::TextDisabled("Baking...");
			}
			else if (ImGui::Button("Bake Beach Level"))
			{
				m_running = true;
				const LevelBakeReport report = BakeBeachLevel(context.services);
				m_running = false;
				if (report.ok)
				{
					char text[512];
					std::snprintf(text, sizeof(text),
					        "Prefab '%s': %d entities.\nInstance overrides kept: %d, dropped: %d%s.\nSave the scene (Ctrl+S) to commit the reference.",
					        report.prefab.c_str(), report.prefabEntities,
					        report.overridesKept, report.overridesDropped,
					        report.replacedInstance ? "" : " (first bake)");
					m_report = text;
				}
				else
				{
					m_report = "Bake failed: " + report.error;
				}
			}
			ImGui::SameLine();
			ImGui::TextDisabled(m_running ? "..." : "idempotent - a re-bake keeps your edits");
		}
		ImGui::End();
	}
} // namespace aether::editor::twinsanity
