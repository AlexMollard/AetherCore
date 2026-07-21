#pragma once

#include <glm/glm.hpp>

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	// Pure helpers (unit-tested); no ECS/Input deps.
	float SliderNormalized(float value, float minValue, float maxValue);
	float SliderQuantize(float value, float minValue, float maxValue, float step);
	float SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float minValue, float maxValue, float step);

	// Drives interactive widgets from focus (UiNavigationSystem) + Input. Runs after nav,
	// before scripts. Reads/writes UISlider/UIToggle runtime state.
	class UiWidgetSystem
	{
	public:
		static void Update(World& world, Input& input, float time);
	};
} // namespace aether::ui
