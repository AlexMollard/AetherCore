#pragma once

#include <cmath>
#include <glm/vec3.hpp>
#include "scene/System.hpp"

namespace aether
{
	class Renderer;
}

namespace aether::app
{
	// Global time-of-day system that drives sun + sky parameters for the frame.
	// Keeps lighting/sky logic out of presentation layers.
	class DayNightSystem final : public aether::System
	{
	public:
		void Init(aether::Renderer& renderer);

		[[nodiscard]] const char* GetName() const override
		{
			return "DayNightSystem";
		}

		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		void SetEnabled(bool enabled)
		{
			m_enabled = enabled;
		}

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		void SetTimeOfDay(float hours)
		{
			m_time = hours * 3600.0f;
		}

		[[nodiscard]] float GetTimeOfDay() const
		{
			return std::fmod(m_time / 3600.0f, 24.0f);
		}

		void SetTimeSpeed(float secondsPerSecond)
		{
			m_timeSpeed = secondsPerSecond;
		}

		[[nodiscard]] float GetTimeSpeed() const
		{
			return m_timeSpeed;
		}

		[[nodiscard]] glm::vec3 GetSunDirection() const
		{
			return m_sunDirection;
		}

	private:
		aether::Renderer* m_renderer = nullptr;
		float m_time = 6.0f * 3600.0f; // start at 6 AM
		float m_timeSpeed = 60.0f;     // 60 simulated seconds per real second
		bool m_enabled = true;
		glm::vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
	};
} // namespace aether::app
