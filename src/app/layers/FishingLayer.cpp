#include "FishingLayer.hpp"

#include <cstdio>

#include "Logger.hpp"
#include "OverlayStyle.hpp"
#include "systems/FishingGameSystem.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"

namespace aether::app
{
	using namespace overlay;

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
		gameSystem->Init(context.engine, *context.assets, *context.cameras, *context.input);
		m_gameSystem = gameSystem.get();
		context.world->RegisterSystem(std::move(gameSystem));
	}

	void FishingLayer::OnDetach(LayerContext& context)
	{
		context.world->UnregisterSystem("FishingGameSystem");
		m_gameSystem = nullptr;
		(void) context;
	}

	void FishingLayer::OnUpdate(LayerContext& context)
	{
		(void) context;
	}

	void FishingLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr)
		{
			return;
		}

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 128> buf{};

		PanelBuilder panel(ui, { 0.f, 0.f }, 12.f, 460.f, 12.f, 110.f);
		panel.Title("FISHING DEMO").Section("GAME");
		panel.KV("Camera", GetActiveCameraName(context.cameras->GetMainCamera()));

		if (m_gameSystem)
		{
			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetFishCount());
			panel.KV("Fish", buf.data());

			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetScore());
			panel.KV("Score", buf.data());

			panel.KV("Bobber", m_gameSystem->GetBobberStateName());
		}

		panel.Section("CONTROLS");
		panel.KV("LMB", "Cast to water");
		panel.KV("Space", "Hook / Reel");
		panel.KV("RMB", "Rotate camera");
		panel.KV("WASD", "Move camera");
	}
} // namespace aether::app
