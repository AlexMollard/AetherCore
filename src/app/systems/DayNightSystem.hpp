#pragma once

#include <glm/vec3.hpp>
#include "scene/System.hpp"

namespace aether
{
	class Renderer;
	class World;
}

namespace aether::app
{
	// Stateless driver over DayNightComponent (scene/LightComponents.hpp): the
	// first entity carrying one owns the animated sun/sky; without one the
	// authored environment stays untouched. The accessor methods below keep the
	// editor panel and script exports stable - they read/write the component.
	class DayNightSystem final : public aether::System
	{
	public:
		static constexpr float kMinTimeSpeed = 0.01f;
		static constexpr float kMaxTimeSpeed = 86400.0f;
		static constexpr float kDefaultTimeSpeed = 60.0f;

		void Init(aether::Renderer& renderer);

		[[nodiscard]] const char* GetName() const override
		{
			return "DayNightSystem";
		}

		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		// Component-backed accessors (no component in the scene = no-ops /
		// component defaults). Kept so the C# exports do not care where the
		// state lives.
		void SetEnabled(bool enabled);
		[[nodiscard]] bool IsEnabled() const;
		void SetTimeOfDay(float hours);
		[[nodiscard]] float GetTimeOfDay() const;
		void SetTimeSpeed(float secondsPerSecond);
		[[nodiscard]] float GetTimeSpeed() const;

		[[nodiscard]] bool HasDriver() const; // any DayNightComponent in the scene?

		[[nodiscard]] glm::vec3 GetSunDirection() const
		{
			return m_sunDirection;
		}

	private:
		aether::Renderer* m_renderer = nullptr;
		aether::World* m_world = nullptr;
		glm::vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
	};
} // namespace aether::app
