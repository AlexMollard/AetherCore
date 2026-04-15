#include "DayNightSystem.hpp"

#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "Renderer.hpp"

namespace aether::app
{
	void DayNightSystem::Init(aether::Renderer& renderer)
	{
		m_renderer = &renderer;
	}

	void DayNightSystem::OnRegister([[maybe_unused]] aether::World& world)
	{
		m_time = 0.0f;
	}

	void DayNightSystem::Update([[maybe_unused]] aether::World& world, float dt)
	{
		if (m_renderer == nullptr)
		{
			return;
		}

		m_time += dt;

		const float sunAngle = m_time * 0.18f;
		const glm::vec3 sunDirection = glm::normalize(glm::vec3(std::cos(sunAngle), 0.35f + 0.85f * std::sin(sunAngle * 0.7f), std::sin(sunAngle)));

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

		const glm::vec3 skyVoid = skyVoidNight * (1.0f - dayFactor) + skyVoidDay * dayFactor;

		m_renderer->SetDirectionalLight(sunDirection, sunIntensity);
		m_renderer->SetSunColor(sunColor);
		m_renderer->SetAmbientLight(ambient);
		m_renderer->SetSkyGradient(skyHorizon, skyZenith);
		m_renderer->SetSkyVoidColor(skyVoid);
	}

	void DayNightSystem::OnUnregister([[maybe_unused]] aether::World& world)
	{
		m_time = 0.0f;
	}
} // namespace aether::app
