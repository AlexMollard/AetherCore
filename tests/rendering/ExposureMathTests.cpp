#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include "rendering/ExposureMath.hpp"

TEST_CASE("Exposure adaptation ignores black bins and preserves an empty meter")
{
	std::array<std::uint32_t, 256> bins{};
	bins[0] = 36864;
	CHECK(aether::AdaptHistogramExposure(bins, 2.0f, 0.18f, 2.0f) == 2.0f);
	bins[128] = 100;
	const float target = 0.18f / std::exp2(-10.0f + 128.0f / 255.0f * 20.0f);
	const float expected = std::exp2(std::log2(2.0f) + (std::log2(target) - std::log2(2.0f)) * (1.0f - std::exp(-2.0f / 60.0f)));
	CHECK(aether::AdaptHistogramExposure(bins, 2.0f, 0.18f, 2.0f) == doctest::Approx(expected));
}

TEST_CASE("Exposure percentile trim rejects bright and dark outliers")
{
	std::array<std::uint32_t, 256> clean{}, outliers{};
	clean[128] = 65;
	outliers[1] = 30;
	outliers[128] = 65;
	outliers[255] = 5;
	CHECK(aether::AdaptHistogramExposure(clean, 1.0f, 0.18f, 2.0f) == doctest::Approx(aether::AdaptHistogramExposure(outliers, 1.0f, 0.18f, 2.0f)));
}

TEST_CASE("Exposure uses fixed logarithmic speed and bounded target")
{
	std::array<std::uint32_t, 256> bins{};
	bins[1] = 100;
	CHECK(aether::AdaptHistogramExposure(bins, 1.0f, 0.18f, 0.0f) == 1.0f);
	CHECK(aether::AdaptHistogramExposure(bins, 1.0f, 0.18f, 100000.0f) == doctest::Approx(30.0f));
	bins[1] = 0;
	bins[255] = 100;
	CHECK(aether::AdaptHistogramExposure(bins, 1.0f, 0.18f, 100000.0f) == doctest::Approx(0.03f));
}
