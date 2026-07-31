#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream> // doctest stringifies the std::string_view returned by SmoothnessLabel
#include <vector>

#include "utils/FrameStats.hpp"
#include "utils/FrameTimeline.hpp"

using namespace aether;

namespace
{
	std::vector<FrameTiming> FramesFrom(const std::vector<float>& wallMs)
	{
		std::vector<FrameTiming> frames;
		frames.reserve(wallMs.size());
		for (std::size_t i = 0; i < wallMs.size(); ++i)
		{
			FrameTiming t;
			t.frameIndex = i;
			t.wallMs = wallMs[i];
			frames.push_back(t);
		}
		return frames;
	}

	std::vector<float> Repeat(const float value, const std::size_t count)
	{
		return std::vector<float>(count, value);
	}
} // namespace

// The panel this replaces could not represent a frame worse than 33.33 ms at all: it
// sampled the simulation delta, which AetherCore clamps to 1/30. These figures come from
// unclamped wall time, so they are free to exceed it.
TEST_CASE("Percentiles are computed on unclamped wall time and exceed 33.33") {
    // Five bad frames in a hundred, so the 99th percentile genuinely lands on one. A
    // single outlier would NOT move P99 - by nearest rank it falls on index 98, still a
    // good frame - and asserting otherwise would be asserting bad statistics.
    std::vector<float> ms = Repeat(16.0f, 95);
    for (int i = 0; i < 5; ++i)
    {
        ms.push_back(250.0f);
    }
    const auto frames = FramesFrom(ms);

    const FrameStats stats = ComputeFrameStats(frames);

    CHECK(stats.sampleCount == 100);
    CHECK(stats.maxMs == doctest::Approx(250.0f));
    CHECK(stats.p99Ms > 33.33f);
    CHECK(stats.minMs == doctest::Approx(16.0f));
    CHECK(stats.medianMs == doctest::Approx(16.0f));
}

// A lone outlier is exactly the case the old panel hid entirely: it would report 33.33.
TEST_CASE("A single bad frame shows up in max even when it cannot move P99") {
    std::vector<float> ms = Repeat(16.0f, 99);
    ms.push_back(250.0f);

    const FrameStats stats = ComputeFrameStats(FramesFrom(ms));

    CHECK(stats.maxMs == doctest::Approx(250.0f));
    CHECK(stats.p99Ms == doctest::Approx(16.0f));
}

TEST_CASE("An even sequence classifies as even") {
    const auto frames = FramesFrom(Repeat(16.67f, 120));

    CHECK(ClassifySmoothness(frames) == Smoothness::Even);
}

// The pattern measured in the editor: burst, then block on the in-flight wait.
TEST_CASE("An alternating short/long sequence classifies as alternating") {
    std::vector<float> ms;
    for (int i = 0; i < 120; ++i)
    {
        ms.push_back(i % 2 == 0 ? 0.4f : 33.0f);
    }
    const auto frames = FramesFrom(ms);

    CHECK(ClassifySmoothness(frames) == Smoothness::Alternating);
}

TEST_CASE("Isolated spikes classify as stuttering, and outrank alternating") {
    std::vector<float> ms = Repeat(16.0f, 119);
    ms.push_back(80.0f); // > 2x median
    const auto frames = FramesFrom(ms);

    CHECK(ClassifySmoothness(frames) == Smoothness::Stuttering);

    // An alternating sequence that ALSO spikes must report the worse of the two.
    std::vector<float> both;
    for (int i = 0; i < 119; ++i)
    {
        both.push_back(i % 2 == 0 ? 0.4f : 33.0f);
    }
    both.push_back(400.0f);
    CHECK(ClassifySmoothness(FramesFrom(both)) == Smoothness::Stuttering);
}

TEST_CASE("Empty input is reported as no samples rather than dividing by zero") {
    const FrameStats stats = ComputeFrameStats({});

    CHECK(stats.sampleCount == 0);
    CHECK(stats.avgMs == doctest::Approx(0.0f));
    CHECK(stats.smoothness == Smoothness::Even);
}

TEST_CASE("SmoothnessLabel names every value") {
    CHECK(SmoothnessLabel(Smoothness::Even) == "even");
    CHECK(SmoothnessLabel(Smoothness::Alternating) == "alternating");
    CHECK(SmoothnessLabel(Smoothness::Stuttering) == "stuttering");
}
