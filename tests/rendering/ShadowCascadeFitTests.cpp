#include <doctest/doctest.h>
#include <ShadowCascadeFit.hpp>

TEST_CASE("Shadow cascade spheres enclose perspective and orthographic slices")
{
	for (const bool orthographic: {false, true})
	{
		for (const float nearZ: {0.1f, 10.0f, 80.0f})
		{
			const float farZ = nearZ + 60;
			const auto sphere = aether::FitShadowCascadeSphere(nearZ, farZ, 1.7f, 0.8f, orthographic);
			for (const float z: {nearZ, farZ})
			{
				const float scale = orthographic ? 1 : z;
				const float distance2 = (1.7f * 1.7f + 0.8f * 0.8f) * scale * scale + (z - sphere.centerDistance) * (z - sphere.centerDistance);
				CHECK(distance2 <= sphere.radius * sphere.radius + 0.01f);
			}
		}
	}
}

TEST_CASE("Orthographic cascade fit retains constant width across depth")
{
	const auto nearSlice = aether::FitShadowCascadeSphere(0.1f, 10.1f, 4, 3, true);
	const auto farSlice = aether::FitShadowCascadeSphere(100, 110, 4, 3, true);
	CHECK(nearSlice.radius == doctest::Approx(farSlice.radius));
	CHECK(farSlice.centerDistance == doctest::Approx(105));
}
