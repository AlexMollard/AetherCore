#include "SandboxLayer.hpp"

#include <string_view>

#include "DebugGui.hpp"

#include "Logger.hpp"
#include "systems/SandboxGameSystem.hpp"
#include "World.hpp"

namespace aether::app
{

	const char* SandboxLayer::GetActiveCameraName(aether::CameraHandle activeCamera) const
	{
		if (!m_gameSystem || !activeCamera.IsValid())
		{
			return "None";
		}
		if (activeCamera == m_gameSystem->GetOrbitCameraHandle())
		{
			return "Orbit";
		}
		if (activeCamera == m_gameSystem->GetFreeCameraHandle())
		{
			return "Free";
		}
		if (activeCamera == m_gameSystem->GetRttCameraHandle())
		{
			return "RTT";
		}
		return "Other";
	}

	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		auto gameSystem = std::make_unique<SandboxGameSystem>();
		gameSystem->Init(context.engine, *context.assets, *context.cameras, *context.input);
		m_gameSystem = gameSystem.get();
		context.world->RegisterSystem(std::move(gameSystem));
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.world->UnregisterSystem("SandboxGameSystem");
		m_gameSystem = nullptr;
		(void) context;
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		(void) context;
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
		ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("SANDBOX", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		ImGui::SeparatorText("SCENE");
		ImGui::Columns(2, "##sc", false);

		ImGui::Text("Camera");
		ImGui::NextColumn();
		ImGui::TextUnformatted(GetActiveCameraName(context.cameras->GetMainCamera()));
		ImGui::NextColumn();

		if (m_gameSystem)
		{
			ImGui::Text("Foxes");
			ImGui::NextColumn();
			ImGui::Text("%zu", m_gameSystem->GetFoxCount());
			ImGui::NextColumn();

			ImGui::Text("Prims/fox");
			ImGui::NextColumn();
			ImGui::Text("%zu", m_gameSystem->GetFoxPrimitiveCount());
			ImGui::NextColumn();

			ImGui::Text("Anims");
			ImGui::NextColumn();
			ImGui::Text("%u", m_gameSystem->GetAnimationCount());
			ImGui::NextColumn();

			const std::string_view anim = m_gameSystem->GetCurrentAnimationName();
			if (!anim.empty())
			{
				ImGui::Text("Playing");
				ImGui::NextColumn();
				ImGui::TextColored(ImVec4(0.40f, 0.72f, 0.46f, 1.f), "%.*s", static_cast<int>(anim.size()), anim.data());
				ImGui::NextColumn();
			}
		}

		ImGui::Columns(1);
		ImGui::End();
	}
} // namespace aether::app
