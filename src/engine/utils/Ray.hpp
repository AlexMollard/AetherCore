#pragma once

#include <algorithm>
#include <limits>

#include <glm/glm.hpp>

namespace aether
{
	struct Ray
	{
		glm::vec3 origin{0.0f};
		glm::vec3 dir{0.0f, 0.0f, -1.0f};
	};

	inline bool RayVsAabb(const Ray& ray, const glm::vec3& mn, const glm::vec3& mx, float& tHit)
	{
		float tMin = 0.0f;
		float tMax = std::numeric_limits<float>::max();
		for (int axis = 0; axis < 3; ++axis)
		{
			if (ray.dir[axis] == 0.0f)
			{
				if (ray.origin[axis] < mn[axis] || ray.origin[axis] > mx[axis])
				{
					return false;
				}
				continue;
			}
			const float invD = 1.0f / ray.dir[axis];
			float t0 = (mn[axis] - ray.origin[axis]) * invD;
			float t1 = (mx[axis] - ray.origin[axis]) * invD;
			if (invD < 0.0f)
			{
				std::swap(t0, t1);
			}
			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);
			if (tMax < tMin)
			{
				return false;
			}
		}
		tHit = tMin;
		return true;
	}

	inline Ray BuildCameraRay(const glm::mat4& invViewProj, glm::vec2 ndc01, const glm::vec3& cameraPos)
	{
		const glm::vec2 ndc = ndc01 * 2.0f - 1.0f;
		glm::vec4 nearPt = invViewProj * glm::vec4(ndc, 0.25f, 1.0f);
		glm::vec4 farPt = invViewProj * glm::vec4(ndc, 0.75f, 1.0f);
		nearPt /= nearPt.w;
		farPt /= farPt.w;
		return Ray{cameraPos, glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt))};
	}
} // namespace aether
