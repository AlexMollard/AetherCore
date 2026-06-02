#include "SandboxLayer.hpp"

#include <array>
#include <cstdio>

#include "AetherCore.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "systems/SandboxGameSystem.hpp"
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
		AE_INFO(LogCategory::App, "Sandbox layer attached.");

		auto gameSystem = std::make_unique<SandboxGameSystem>();
		gameSystem->Init(context.services, context.Get<AssetManager>(), context.Get<CameraManager>(), context.Get<Input>());
		m_gameSystem = gameSystem.get();
		context.Get<World>().RegisterSystem(std::move(gameSystem));
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.Get<World>().UnregisterSystem("SandboxGameSystem");
		m_gameSystem = nullptr;
		(void) context;
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		m_activeCamera = context.Get<CameraManager>().GetMainCamera();
		if (m_gameSystem)
		{
			m_foxCount = m_gameSystem->GetFoxCount();
			m_primCount = m_gameSystem->GetFoxPrimitiveCount();
			m_animCount = m_gameSystem->GetAnimationCount();
			const std::string_view anim = m_gameSystem->GetCurrentAnimationName();
			m_animName = anim;
		}
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
		UIRenderer& ui = context.Get<UIRenderer>();

		constexpr float kPanelW = 260.f;
		constexpr float kPadX = 14.f;
		constexpr float kPadY = 10.f;
		constexpr float kRowH = 18.f;
		constexpr float kSepH = 12.f;

		const glm::vec4 bg{0.08f, 0.08f, 0.11f, 0.92f};
		const glm::vec4 white{0.93f, 0.93f, 0.93f, 1.f};
		const glm::vec4 green{0.40f, 0.72f, 0.46f, 1.f};

		// Calculate panel height
		float contentH = kPadY;
		contentH += kRowH; // Camera
		contentH += kSepH;
		contentH += kRowH; // Foxes
		contentH += kRowH; // Prims/fox
		contentH += kRowH; // Anims
		if (!m_animName.empty())
		{
			contentH += kRowH; // Playing
		}
		contentH += kPadY;

		ui.DrawRect(PxRect(12.f, 12.f, 12.f + kPanelW, 12.f + contentH), bg, 6.f);

		float y = 12.f + kPadY;
		const float col2X = 12.f + 160.f;
		const float textSize = 13.f;

		auto label = [&](const char* name, const char* value, glm::vec4 valueColor)
		{
			ui.DrawText(name, {.anchor = {0.f, 0.f}, .offsetPx = {12.f + kPadX, y}}, textSize, white);
			ui.DrawText(value, {.anchor = {0.f, 0.f}, .offsetPx = {col2X, y}}, textSize, valueColor);
			y += kRowH;
		};

		label("Camera", GetActiveCameraName(m_activeCamera), white);

		y += 4.f;

		std::array<char, 32> buf{};
		std::snprintf(buf.data(), buf.size(), "%zu", m_foxCount);
		label("Foxes", buf.data(), white);
		std::snprintf(buf.data(), buf.size(), "%zu", m_primCount);
		label("Prims/fox", buf.data(), white);
		std::snprintf(buf.data(), buf.size(), "%u", m_animCount);
		label("Anims", buf.data(), white);

		if (!m_animName.empty())
		{
			label("Playing", m_animName.c_str(), green);
		}
	}
} // namespace aether::app
