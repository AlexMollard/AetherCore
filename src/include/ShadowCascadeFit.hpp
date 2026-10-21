#pragma once

#include <algorithm>
#include <cmath>

namespace aether
{
	struct ShadowCascadeSphere
	{
		float centerDistance;
		float radius;
	};

	// Projection diagonals provide half extents for orthographic cameras and
	// half-angle tangents for perspective cameras.
	inline ShadowCascadeSphere FitShadowCascadeSphere(float nearZ, float farZ, float halfX, float halfY, bool orthographic)
	{
		const float a2 = halfX * halfX + halfY * halfY;
		if (orthographic)
		{
			const float halfDepth = (farZ - nearZ) * 0.5f;
			return {(nearZ + farZ) * 0.5f, std::sqrt(a2 + halfDepth * halfDepth)};
		}
		const float center = (farZ + nearZ) * (a2 + 1.0f) * 0.5f;
		if (center >= farZ)
		{
			return {farZ, farZ * std::sqrt(a2)};
		}
		const float delta = nearZ - center;
		return {center, std::sqrt(nearZ * nearZ * a2 + delta * delta)};
	}
} // namespace aether
