#include "ui/UiWidgetSystem.hpp"

#include <algorithm>
#include <cmath>

namespace aether::ui
{
	float SliderNormalized(float value, float minValue, float maxValue)
	{
		const float range = maxValue - minValue;
		if (range <= 1e-6f)
		{
			return 0.f;
		}
		return std::clamp((value - minValue) / range, 0.f, 1.f);
	}

	float SliderQuantize(float value, float minValue, float maxValue, float step)
	{
		float v = std::clamp(value, minValue, maxValue);
		if (step > 1e-6f)
		{
			v = minValue + std::round((v - minValue) / step) * step;
			v = std::clamp(v, minValue, maxValue);
		}
		return v;
	}

	float SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float minValue, float maxValue, float step)
	{
		const float pad = 2.f;
		const float innerX = trackRect.x + pad;
		const float innerW = std::max(trackRect.z - 2.f * pad, 1e-6f);
		const float t = std::clamp((mouseX - innerX) / innerW, 0.f, 1.f);
		return SliderQuantize(minValue + t * (maxValue - minValue), minValue, maxValue, step);
	}

	void UiWidgetSystem::Update(World&, Input&, float) {}
} // namespace aether::ui
