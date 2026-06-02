#include "FishingLayer.hpp"

#include <array>
#include <cstdio>

#include "assets/AssetManager.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "systems/FishingGameSystem.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	namespace
	{
		UiRect PxRect(float l, float t, float r, float b)
		{
			return UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {0.f, 0.f}, .offsetMinPx = {l, t}, .offsetMaxPx = {r, b}};
		}
	} // namespace

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
		AE_INFO(LogCategory::App, "Fishing layer attached.");

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
		m_activeCamera = context.Get<CameraManager>().GetMainCamera();
		if (m_gameSystem)
		{
			m_fishCount = m_gameSystem->GetFishCount();
			m_score = m_gameSystem->GetScore();
			m_bobberState = m_gameSystem->GetBobberStateName();
		}
	}

	void FishingLayer::OnGui(LayerContext& context)
	{
		UIRenderer& ui = context.Get<UIRenderer>();

		constexpr float kPanelW = 280.f;
		constexpr float kPadX = 14.f;
		constexpr float kPadY = 10.f;
		constexpr float kRowH = 18.f;
		constexpr float kSepH = 12.f;

		const glm::vec4 bg{0.08f, 0.08f, 0.11f, 0.92f};
		const glm::vec4 white{0.93f, 0.93f, 0.93f, 1.f};
		const glm::vec4 yellow{0.86f, 0.71f, 0.30f, 1.f};

		// Calculate panel height
		float contentH = kPadY;
		// GAME section
		contentH += kRowH; // Camera
		contentH += kRowH; // Fish
		contentH += kRowH; // Score
		contentH += kRowH; // Bobber
		contentH += kSepH;
		// CONTROLS section
		contentH += kRowH; // LMB
		contentH += kRowH; // Space
		contentH += kRowH; // RMB
		contentH += kRowH; // WASD
		contentH += kPadY;

		ui.DrawRect(PxRect(12.f, 12.f, 12.f + kPanelW, 12.f + contentH), bg, 6.f);

		float y = 12.f + kPadY;
		const float col2X = 12.f + 160.f;
		const float colCtrlX = 12.f + 80.f;
		const float textSize = 13.f;

		std::array<char, 64> buf{};

		auto label = [&](const char* name, const char* value, glm::vec4 valueColor)
		{
			ui.DrawText(name, {.anchor = {0.f, 0.f}, .offsetPx = {12.f + kPadX, y}}, textSize, white);
			ui.DrawText(value, {.anchor = {0.f, 0.f}, .offsetPx = {col2X, y}}, textSize, valueColor);
			y += kRowH;
		};

		auto ctrl = [&](const char* key, const char* desc)
		{
			ui.DrawText(key, {.anchor = {0.f, 0.f}, .offsetPx = {12.f + kPadX, y}}, textSize, yellow);
			ui.DrawText(desc, {.anchor = {0.f, 0.f}, .offsetPx = {colCtrlX, y}}, textSize, white);
			y += kRowH;
		};

		label("Camera", GetActiveCameraName(m_activeCamera), white);

		y += 4.f;

		std::snprintf(buf.data(), buf.size(), "%zu", m_fishCount);
		label("Fish", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%zu", m_score);
		label("Score", buf.data(), white);

		label("Bobber", m_bobberState, white);

		y += 4.f;

		ctrl("LMB", "Cast to water");
		ctrl("Space", "Hook / Reel");
		ctrl("RMB", "Rotate camera");
		ctrl("WASD", "Move camera");
	}
} // namespace aether::app
