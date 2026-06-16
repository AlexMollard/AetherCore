#include "PhysicsLayer.hpp"

#include <array>
#include <cstdio>

#include "assets/AssetManager.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "systems/PhysicsGameSystem.hpp"
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

	void PhysicsLayer::OnAttach(LayerContext& context)
	{
		AE_INFO(aether::LogCategory::App, "PhysicsLayer attached.");

		auto gameSystem = std::make_unique<PhysicsGameSystem>();
		gameSystem->Init(context.services, context.Get<AssetManager>(), context.Get<CameraManager>(), context.Get<Input>());
		m_gameSystem = gameSystem.get();
		context.Get<World>().RegisterSystem(std::move(gameSystem));
	}

	void PhysicsLayer::OnDetach(LayerContext& context)
	{
		context.Get<World>().UnregisterSystem("PhysicsGameSystem");
		m_gameSystem = nullptr;
	}

	void PhysicsLayer::OnUpdate([[maybe_unused]] LayerContext& context)
	{
		if (m_gameSystem)
		{
			m_activeBodyCount = m_gameSystem->GetActiveBodyCount();
			m_projectileCount = m_gameSystem->GetProjectileCount();
			m_simTime = m_gameSystem->GetSimTime();
		}
	}

	void PhysicsLayer::OnGui([[maybe_unused]] LayerContext& context)
	{
		if (!m_gameSystem)
		{
			return;
		}

		auto& ui = context.Get<UIRenderer>();

		constexpr float kPanelW = 280.f;
		constexpr float kPadX = 14.f;
		constexpr float kPadY = 10.f;
		constexpr float kRowH = 18.f;
		constexpr float kSepH = 12.f;

		const glm::vec4 bg{0.08f, 0.08f, 0.11f, 0.92f};
		const glm::vec4 white{0.93f, 0.93f, 0.93f, 1.f};
		const glm::vec4 green{0.40f, 0.72f, 0.46f, 1.f};
		const glm::vec4 yellow{0.86f, 0.71f, 0.30f, 1.f};

		// Calculate panel height
		float contentH = kPadY;
		// SIMULATION section
		contentH += kRowH; // Dynamic bodies
		contentH += kRowH; // Projectiles
		contentH += kRowH; // Sim time
		contentH += kRowH; // Step rate
		contentH += kSepH;
		// CONTROLS section
		contentH += kRowH; // Space
		contentH += kRowH; // R
		contentH += kRowH; // C
		contentH += kPadY;

		ui.DrawRect(PxRect(12.f, 12.f, 12.f + kPanelW, 12.f + contentH), bg, 6.f);

		float y = 12.f + kPadY;
		const float col2X = 12.f + 180.f;
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

		std::snprintf(buf.data(), buf.size(), "%d", m_activeBodyCount);
		label("Dynamic bodies", buf.data(), white);
		std::snprintf(buf.data(), buf.size(), "%d", m_projectileCount);
		label("Projectiles", buf.data(), white);
		std::snprintf(buf.data(), buf.size(), "%.1f s", m_simTime);
		label("Sim time", buf.data(), white);
		label("Step rate", "60.0 Hz (fixed)", green);

		y += 4.f;

		ctrl("Space", "Fire projectile");
		ctrl("R", "Reset scene");
		ctrl("C", "Toggle camera");
	}
} // namespace aether::app
