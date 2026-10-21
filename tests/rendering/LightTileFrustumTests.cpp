#include <doctest/doctest.h>

#include "../../src/shaders/include/LightTileFrustum.slangh"

TEST_CASE("Orthographic tiled lights retain a cone that reaches an offset tile")
{
	using namespace aether::lighting;
	const auto planes = BuildLightTilePlanes(-3, -2.5f, -0.25f, 0.25f, true);
	CHECK(LightTileIntersectsCone(planes, {1.5f, 0, 0}, {0, 0, -1}, 12, std::tan(1.0f)));
	const auto outside = BuildLightTilePlanes(25, 26, -0.25f, 0.25f, true);
	CHECK_FALSE(LightTileIntersectsCone(outside, {1.5f, 0, 0}, {0, 0, -1}, 12, std::tan(1.0f)));
}

TEST_CASE("Tiled light planes do not reject sampled points inside their spot cone")
{
	using namespace aether::lighting;
	const float3 apex(1.5f, 0, 0), direction(0, 0, -1);
	for (const bool orthographic: {false, true})
	{
		for (const float depth: {2.0f, 4.0f, 8.0f})
		{
			for (float x = -6; x <= 6; x += 0.5f)
			{
				const float3 point(x, 0.5f, -depth);
				if (glm::dot(glm::normalize(point - apex), direction) < std::cos(1.0f))
				{
					continue;
				}
				const float factor = orthographic ? 1 : 1 / depth;
				const auto planes = BuildLightTilePlanes((x - 0.125f) * factor, (x + 0.125f) * factor, 0.375f * factor, 0.625f * factor, orthographic);
				CHECK(LightTileIntersectsCone(planes, apex, direction, 12, std::tan(1.0f)));
			}
		}
	}
}
