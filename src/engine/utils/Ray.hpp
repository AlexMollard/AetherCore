#pragma once

#include <algorithm>
#include <limits>

#include <glm/glm.hpp>

namespace aether
{
	// World- or object-space ray. dir need not be normalized for RayVsAabb (tHit
	// is then in units of dir length); BuildCameraRay returns a normalized dir.
	struct Ray
	{
		glm::vec3 origin{0.0f};
		glm::vec3 dir{0.0f, 0.0f, -1.0f};
	};

	// Slab test. On hit, tHit >= 0 is the entry distance along dir (0 when the
	// origin starts inside). Axis-parallel rays are handled explicitly - the
	// naive (bound - origin) * (1/0) form produces NaN when the origin sits
	// exactly on a slab plane.
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

	// Ray through a viewport pixel. ndc01 is the image-space UV in [0,1]^2 with
	// (0,0) at the TOP-LEFT of the displayed image. The engine's projection
	// carries the Vulkan Y-flip (proj[1][1] *= -1), so clip-space y already
	// points down and no extra sign flip is needed here - the round-trip
	// doctest in tests/utils/RayTests.cpp pins this convention.
	//
	// Any two distinct clip depths unproject to points on the same pixel ray
	// (projective line), which makes this independent of the GLM depth-range
	// convention; with a standard (non-reversed) projection the larger depth is
	// farther, so the direction points away from the camera.
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
