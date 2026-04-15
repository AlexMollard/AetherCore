#include "SandboxLayer.hpp"

#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <string_view>

#include "Logger.hpp"
#include "systems/SandboxGameSystem.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"
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

	void SandboxLayer::DrawDebugLine(aether::UIRenderer& ui, std::string_view text, float y) const
	{
		ui.DrawText(text,
		        aether::UiPoint{
		                .anchor = {  0.0f, 0.0f },
		                .offsetPx = { 24.0f,    y },
        },
		        18.0f,
		        glm::vec4(0.92f, 0.95f, 0.97f, 1.0f));
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
		{
			return;
		}

		context.ui->DrawQuad(
		        aether::UiRect{
		                .anchorMin = {   0.0f,   0.0f },
		                .anchorMax = {   0.0f,   0.0f },
		                .offsetMinPx = {  12.0f,  12.0f },
		                .offsetMaxPx = { 410.0f, 248.0f },
        },
		        glm::vec4(0.08f, 0.11f, 0.14f, 0.82f));

		context.ui->DrawText("Sandbox Debug",
		        aether::UiPoint{
		                .anchor = {  0.0f,  0.0f },
		                .offsetPx = { 24.0f, 42.0f },
        },
		        28.0f,
		        glm::vec4(0.95f, 0.90f, 0.68f, 1.0f));

		std::array<char, 128> line{};
		const char* activeCameraName = GetActiveCameraName(context.cameras->GetMainCamera());

		std::snprintf(line.data(), line.size(), "Camera: %s", activeCameraName);
		DrawDebugLine(*context.ui, line.data(), 82.0f);

		if (m_gameSystem)
		{
			std::snprintf(line.data(), line.size(), "Scene: %zu ring, %zu model prims, %u anims", m_gameSystem->GetRingCount(), m_gameSystem->GetModelPrimitiveCount(), m_gameSystem->GetAnimationCount());
			DrawDebugLine(*context.ui, line.data(), 106.0f);
		}

		if (m_gameSystem)
		{
			const std::string_view currentAnimation = m_gameSystem->GetCurrentAnimationName();
			if (!currentAnimation.empty())
			{
				context.ui->DrawText(std::string("Anim: ") + std::string(currentAnimation),
				        aether::UiPoint{
				                .anchor = {  0.0f,   0.0f },
				                .offsetPx = { 24.0f, 130.0f },
                },
				        18.0f,
				        glm::vec4(0.74f, 0.86f, 0.76f, 1.0f));
			}
		}
	}
} // namespace aether::app
