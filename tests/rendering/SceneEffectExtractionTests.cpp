#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/SceneEffectExtraction.hpp"

using namespace aether;

TEST_CASE("Shared camera effects preserve thin-lens focus and aperture semantics")
{
	const auto lens = ExtractDepthOfFieldParams(10.0f, 50.0f, 5.6f, true, true);
	CHECK(lens.x == doctest::Approx(10.0f));
	CHECK(lens.z == doctest::Approx(24.0f));
	CHECK(lens.w == doctest::Approx(1.0f));
	const auto blur = [&](float distance)
	{
		return lens.y * std::abs(distance - lens.x) / distance;
	};
	CHECK(blur(10.0f) == doctest::Approx(0.0f));
	CHECK(blur(5.0f) > 0.0f);
	CHECK(ExtractDepthOfFieldParams(10.0f, 50.0f, 2.8f, true, true).y == doctest::Approx(lens.y * 2.0f));
	CHECK(ExtractDepthOfFieldParams(10.0f, 100.0f, 5.6f, true, true).y > lens.y * 4.0f);
	CHECK(ExtractDepthOfFieldParams(10.0f, 50.0f, 5.6f, false, true).w == doctest::Approx(0.0f));
	CHECK(ExtractDepthOfFieldParams(10.0f, 50.0f, 5.6f, true, false).w == doctest::Approx(0.0f));
	const auto clamped = ExtractDepthOfFieldParams(0.0f, 0.0f, 0.0f, true, true);
	CHECK(std::isfinite(clamped.y));
	CHECK(clamped.x > 0.0f);
}

TEST_CASE("Shared effect bounds transform the cooked sphere with maximum axis scale")
{
	const auto world = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 3.0f, -2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, -3.0f, 1.0f));
	const auto sphere = TransformEffectBoundingSphere(glm::vec4(1.0f, 1.0f, 2.0f, 0.5f), world);
	CHECK(sphere.x == doctest::Approx(7.0f));
	CHECK(sphere.y == doctest::Approx(0.0f));
	CHECK(sphere.z == doctest::Approx(0.0f));
	CHECK(sphere.w == doctest::Approx(1.5f));
}

TEST_CASE("Shared blobs merge actor primitives and retain the native signed pickup radius")
{
	const std::array sources{BlobShadowSource{1, glm::vec4(-1.0f, 2.0f, 0.0f, 0.5f), false}, BlobShadowSource{1, glm::vec4(1.0f, 2.0f, 0.0f, 0.5f), false}, BlobShadowSource{2, glm::vec4(0.0f, 1.0f, 3.0f, 0.5f), true}};
	std::vector<glm::vec4> result;
	CHECK(ExtractBlobShadows(sources, glm::vec3(0.0f), 64, result) == 1);
	REQUIRE(result.size() == 2);
	CHECK(result[0].z == doctest::Approx(3.0f));
	CHECK(result[0].w == doctest::Approx(-0.25f));
	CHECK(result[1].x == doctest::Approx(0.0f));
	CHECK(result[1].y == doctest::Approx(1.5f));
	CHECK(result[1].w == doctest::Approx(0.75f));
	CHECK(ExtractBlobShadows(sources, glm::vec3(0.0f), 1, result) == 1);
	REQUIRE(result.size() == 1);
	CHECK(result[0].z == doctest::Approx(3.0f));
}

TEST_CASE("Shared darkness volumes discard degenerate boxes and cap by eye distance")
{
	const std::array sources{
	        DarknessVolumeSource{glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, 0.0f, 0.0f)), 3.0f}, DarknessVolumeSource{glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)), 5.0f}, DarknessVolumeSource{glm::mat4(0.0f), 1.0f}};
	std::vector<glm::vec4> result;
	ExtractDarknessVolumes(sources, glm::vec3(0.0f), 1, result);
	REQUIRE(result.size() == 2);
	CHECK(result[0].x == doctest::Approx(2.0f));
	CHECK(result[0].w == doctest::Approx(5.0f));
	CHECK(result[1].x == doctest::Approx(0.5f));
	CHECK(result[1].w == doctest::Approx(0.0f));
}
