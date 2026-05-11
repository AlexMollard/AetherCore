#pragma once

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

		const char* GetName() const override
		{
			return "DayNightSystem";
		}

		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

	private:
		aether::Renderer* m_renderer = nullptr;
		float m_time = 0.0f;
	};
} // namespace aether::app
