#include "FishingLayer.hpp"

#include "utils/DebugGui.hpp"

#include "utils/Logger.hpp"
#include "systems/FishingGameSystem.hpp"

namespace aether::app
{

	const char* FishingLayer::GetActiveCameraName(aether::CameraHandle activeCamera) const
	{
		if (!activeCamera.IsValid())
		{
			return "None";
		}

		return "Fishing";
	}

	void FishingLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Fishing layer attached.");

		auto gameSystem = std::make_unique<FishingGameSystem>();
		gameSystem->Init(context.services, context.Get<AssetManager>(), context.Get<CameraManager>(), context.Get<Input>());
		m_gameSystem = gameSystem.get();
		context.Get<World>().RegisterSystem(std::move(gameSystem));
	}

	void FishingLayer::OnDetach(LayerContext& context)
	{
		context.Get<World>().UnregisterSystem("FishingGameSystem");
		m_gameSystem = nullptr;
		(void) context;
	}

	void FishingLayer::OnUpdate(LayerContext& context)
	{
		(void) context;
	}

	void FishingLayer::OnGui(LayerContext& context)
	{
		ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("FISHING DEMO", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		ImGui::SeparatorText("GAME");
		ImGui::Columns(2, "##gm", false);

		ImGui::Text("Camera");
		ImGui::NextColumn();
		ImGui::TextUnformatted(GetActiveCameraName(context.Get<CameraManager>().GetMainCamera()));
		ImGui::NextColumn();

		if (m_gameSystem)
		{
			ImGui::Text("Fish");
			ImGui::NextColumn();
			ImGui::Text("%zu", m_gameSystem->GetFishCount());
			ImGui::NextColumn();

			ImGui::Text("Score");
			ImGui::NextColumn();
			ImGui::Text("%zu", m_gameSystem->GetScore());
			ImGui::NextColumn();

			ImGui::Text("Bobber");
			ImGui::NextColumn();
			ImGui::TextUnformatted(m_gameSystem->GetBobberStateName());
			ImGui::NextColumn();
		}

		ImGui::Columns(1);
		ImGui::SeparatorText("CONTROLS");
		ImGui::Columns(2, "##ctrl", false);

		ImGui::Text("LMB");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Cast to water");
		ImGui::NextColumn();
		ImGui::Text("Space");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Hook / Reel");
		ImGui::NextColumn();
		ImGui::Text("RMB");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Rotate camera");
		ImGui::NextColumn();
		ImGui::Text("WASD");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Move camera");
		ImGui::NextColumn();

		ImGui::Columns(1);
		ImGui::End();
	}
} // namespace aether::app
