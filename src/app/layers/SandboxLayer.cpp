#include "SandboxLayer.hpp"

#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <string_view>

#include "Logger.hpp"
#include "OverlayStyle.hpp"
#include "systems/SandboxGameSystem.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"
#include "World.hpp"

namespace aether::app
{
	using namespace overlay;

	const char* SandboxLayer::GetActiveCameraName(aether::CameraHandle activeCamera) const
	{
		if (!m_gameSystem || !activeCamera.IsValid())
			return "None";
		if (activeCamera == m_gameSystem->GetOrbitCameraHandle())
			return "Orbit";
		if (activeCamera == m_gameSystem->GetFreeCameraHandle())
			return "Free";
		if (activeCamera == m_gameSystem->GetRttCameraHandle())
			return "RTT";
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
		if (context.ui == nullptr)
			return;

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 128> buf{};

		PanelBuilder panel(ui, { 0.f, 0.f }, 12.f, 388.f, 12.f, 110.f);
		panel.Title("SANDBOX").Section("SCENE");
		panel.KV("Camera", GetActiveCameraName(context.cameras->GetMainCamera()));

		if (m_gameSystem)
		{
			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetFoxCount());
			panel.KV("Foxes", buf.data());

			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetFoxPrimitiveCount());
			panel.KV("Prims/fox", buf.data());

			std::snprintf(buf.data(), buf.size(), "%u", m_gameSystem->GetAnimationCount());
			panel.KV("Anims", buf.data());

			const std::string_view anim = m_gameSystem->GetCurrentAnimationName();
			if (!anim.empty())
				panel.KV("Playing", anim, kColorGood);
		}
	}
} // namespace aether::app
