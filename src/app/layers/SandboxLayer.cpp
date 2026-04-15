#include "SandboxLayer.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "Logger.hpp"
#include "UIRenderer.hpp"
#include "UiLayout.hpp"
#include "World.hpp"
#include "systems/SandboxGameSystem.hpp"

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
		ui.DrawText(
			text,
			aether::UiPoint{
				.anchor = { 0.0f, 0.0f },
				.offsetPx = { 24.0f, y },
			},
			18.0f,
			glm::vec4(0.92f, 0.95f, 0.97f, 1.0f));
	}

	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		// Configure default lighting and sky controls.
		context.renderer->SetDirectionalLight(glm::vec3(0.35f, 0.88f, 0.31f), 4.5f);
		context.renderer->SetSunColor(glm::vec3(1.0f, 0.96f, 0.90f));
		context.renderer->SetAmbientLight(glm::vec3(0.12f, 0.13f, 0.15f));
		context.renderer->SetSkyGradient(
			glm::vec3(0.34f, 0.52f, 0.82f),
			glm::vec3(0.08f, 0.19f, 0.45f));
		context.renderer->SetSkyVoidColor(glm::vec3(0.001f, 0.002f, 0.005f));
		m_lightTime = 0.0f;

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
		(void)context;
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		m_lightTime += static_cast<float>(context.deltaTimeSeconds) * 3.0f;

		const float sunAngle = m_lightTime * 0.18f;
		const glm::vec3 sunDirection = glm::normalize(glm::vec3(
			std::cos(sunAngle),
			0.35f + 0.85f * std::sin(sunAngle * 0.7f),
			std::sin(sunAngle)));

		const float dayFactor = glm::smoothstep(-0.05f, 0.35f, sunDirection.y);
		const float horizonFactor = 1.0f - glm::smoothstep(0.0f, 0.50f, std::abs(sunDirection.y));
		const float dawnFactor = horizonFactor * (1.0f - dayFactor * 0.6f);

		const float sunIntensity = 0.45f + (5.2f - 0.45f) * dayFactor;

		const glm::vec3 ambientNight = { 0.010f, 0.012f, 0.018f };
		const glm::vec3 ambientDay = { 0.120f, 0.130f, 0.150f };
		const glm::vec3 ambientDawn = { 0.220f, 0.135f, 0.080f };
		const glm::vec3 sunNight = { 0.08f, 0.10f, 0.18f };
		const glm::vec3 sunDay = { 1.00f, 0.96f, 0.90f };
		const glm::vec3 sunDawn = { 1.25f, 0.62f, 0.32f };
		const glm::vec3 skyHorizonNight = { 0.015f, 0.020f, 0.040f };
		const glm::vec3 skyHorizonDay = { 0.34f, 0.52f, 0.82f };
		const glm::vec3 skyHorizonDawn = { 0.78f, 0.38f, 0.16f };
		const glm::vec3 skyZenithNight = { 0.004f, 0.008f, 0.018f };
		const glm::vec3 skyZenithDay = { 0.08f, 0.19f, 0.45f };
		const glm::vec3 skyZenithDawn = { 0.18f, 0.12f, 0.28f };
		const glm::vec3 skyVoidNight = { 0.0004f, 0.0008f, 0.0018f };
		const glm::vec3 skyVoidDay = { 0.0015f, 0.0020f, 0.0040f };

		glm::vec3 ambient = ambientNight * (1.0f - dayFactor) + ambientDay * dayFactor;
		ambient = ambient * (1.0f - dawnFactor) + ambientDawn * dawnFactor;

		glm::vec3 sunColor = sunNight * (1.0f - dayFactor) + sunDay * dayFactor;
		sunColor = sunColor * (1.0f - dawnFactor) + sunDawn * dawnFactor;

		glm::vec3 skyHorizon = skyHorizonNight * (1.0f - dayFactor) + skyHorizonDay * dayFactor;
		skyHorizon = skyHorizon * (1.0f - dawnFactor) + skyHorizonDawn * dawnFactor;

		glm::vec3 skyZenith = skyZenithNight * (1.0f - dayFactor) + skyZenithDay * dayFactor;
		skyZenith = skyZenith * (1.0f - dawnFactor) + skyZenithDawn * dawnFactor;

		glm::vec3 skyVoid = skyVoidNight * (1.0f - dayFactor) + skyVoidDay * dayFactor;

		context.renderer->SetDirectionalLight(sunDirection, sunIntensity);
		context.renderer->SetSunColor(sunColor);
		context.renderer->SetAmbientLight(ambient);
		context.renderer->SetSkyGradient(skyHorizon, skyZenith);
		context.renderer->SetSkyVoidColor(skyVoid);
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr)
		{
			return;
		}

		context.ui->DrawQuad(
			aether::UiRect{
				.anchorMin = { 0.0f, 0.0f },
				.anchorMax = { 0.0f, 0.0f },
				.offsetMinPx = { 12.0f, 12.0f },
				.offsetMaxPx = { 410.0f, 248.0f },
			},
			glm::vec4(0.08f, 0.11f, 0.14f, 0.82f));

		context.ui->DrawText(
			"Sandbox Debug",
			aether::UiPoint{
				.anchor = { 0.0f, 0.0f },
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
			std::snprintf(line.data(), line.size(), "Scene: %zu ring, %zu model prims, %u anims",
				m_gameSystem->GetRingCount(),
				m_gameSystem->GetModelPrimitiveCount(),
				m_gameSystem->GetAnimationCount());
			DrawDebugLine(*context.ui, line.data(), 106.0f);
		}

		if (m_gameSystem)
		{
			const std::string_view currentAnimation = m_gameSystem->GetCurrentAnimationName();
			if (!currentAnimation.empty())
			{
				context.ui->DrawText(
					std::string("Anim: ") + std::string(currentAnimation),
					aether::UiPoint{
						.anchor = { 0.0f, 0.0f },
						.offsetPx = { 24.0f, 130.0f },
					},
					18.0f,
					glm::vec4(0.74f, 0.86f, 0.76f, 1.0f));
			}
		}
	}
}
