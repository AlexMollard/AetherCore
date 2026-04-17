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
	namespace
	{
		using namespace overlay;

		// Panel geometry — left-anchored at (0,0)
		constexpr glm::vec2 kAnchor{ 0.f, 0.f };
		constexpr float kPanelL = 12.0f;
		constexpr float kPanelR = 388.0f;
		constexpr float kPanelTop = 12.0f;
		constexpr float kPanelBot = 220.0f;
		constexpr float kInnerL = kPanelL + kPad;
		constexpr float kInnerR = kPanelR - kPad;

		constexpr float kColKey = kInnerL;
		constexpr float kColVal = kInnerL + 110.0f;
	} // namespace

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

		// Create and initialize the game system.
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

		DrawPanel(ui, kAnchor, kPanelL, kPanelR, kPanelTop, kPanelBot);

		// Title
		ui.DrawText("SANDBOX",
		        aether::UiPoint{
		                .anchor = kAnchor, .offsetPx = { kInnerL, kPanelTop + 26.0f }
        },
		        18.0f,
		        kColorTitle);

		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kPanelTop + 54.0f);

		constexpr float kSceneY = kPanelTop + 66.0f;
		DrawSectionHeader(ui, "SCENE", kAnchor, kPanelL, kInnerL, kSceneY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kSceneY + 13.0f);

		constexpr float kR1 = kSceneY + 34.0f;
		const char* camName = GetActiveCameraName(context.cameras->GetMainCamera());
		DrawKV(ui, "Camera", camName, kAnchor, kColKey, kColVal, kR1);

		if (m_gameSystem)
		{
			constexpr float kR2 = kR1 + kRowH;
			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetFoxCount());
			DrawKV(ui, "Foxes", buf.data(), kAnchor, kColKey, kColVal, kR2);

			constexpr float kR3 = kR2 + kRowH;
			std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetFoxPrimitiveCount());
			DrawKV(ui, "Prims/fox", buf.data(), kAnchor, kColKey, kColVal, kR3);

			constexpr float kR4 = kR3 + kRowH;
			std::snprintf(buf.data(), buf.size(), "%u", m_gameSystem->GetAnimationCount());
			DrawKV(ui, "Anims", buf.data(), kAnchor, kColKey, kColVal, kR4);

			const std::string_view anim = m_gameSystem->GetCurrentAnimationName();
			if (!anim.empty())
			{
				constexpr float kR5 = kR4 + kRowH;
				DrawKV(ui, "Playing", anim, kAnchor, kColKey, kColVal, kR5, kColorGood);
			}
		}
	}
} // namespace aether::app
