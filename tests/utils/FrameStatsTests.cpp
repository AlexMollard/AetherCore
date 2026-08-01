#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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

// Measured on a Release editor running at 1494 fps: median 0.61 ms with one 3.29 ms
// frame. Relatively that is a 5x spike; absolutely it is invisible. Reporting that as
// stuttering is how a diagnostic loses its credibility for the cases that matter.
TEST_CASE("A relatively large but absolutely tiny spike is not stuttering") {
    std::vector<float> ms = Repeat(0.61f, 119);
    ms.push_back(3.29f);

    CHECK(ClassifySmoothness(FramesFrom(ms)) == Smoothness::Even);
}

// The same shape at 60 Hz IS worth reporting: the excursion clears the absolute floor.
TEST_CASE("The same relative spike at 60 fps is still stuttering") {
    std::vector<float> ms = Repeat(16.0f, 119);
    ms.push_back(64.0f);

    CHECK(ClassifySmoothness(FramesFrom(ms)) == Smoothness::Stuttering);
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

// The published game reports over the whole ring (600 frames); the editor panel only ever
// passes 240 and every other test here uses 120. A Debug GameRuntime tripped
// "stack around the variable 'sorted' was corrupted" inside ComputeFrameStats, so pin the
// full-ring size explicitly.
TEST_CASE("Stats over a full timeline ring do not corrupt the stack") {
    std::vector<float> ms;
    ms.reserve(600);
    for (std::size_t i = 0; i < 600; ++i)
    {
        ms.push_back(16.0f + static_cast<float>(i % 7));
    }

    const FrameStats stats = ComputeFrameStats(FramesFrom(ms));

    CHECK(stats.sampleCount == 600);
    CHECK(stats.minMs == doctest::Approx(16.0f));
    CHECK(stats.maxMs == doctest::Approx(22.0f));
}

// Mirrors the in-engine probe EXACTLY - a 64-element std::array on the STACK, spanned - not
// a heap vector like every other case here. A Debug GameRuntime trips /RTCs "stack around
// the variable 'sorted' was corrupted" on this call even single-threaded in the constructor,
// against the same Engine.lib this binary links, so pin the exact shape.
TEST_CASE("Stats over a stack array of 64 frames do not corrupt the stack") {
    std::array<FrameTiming, 64> frames{};
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        frames[i].frameIndex = i;
        frames[i].wallMs = 16.0f + static_cast<float>(i % 5);
        frames[i].simDtMs = 16.0f;
    }

    const FrameStats stats = ComputeFrameStats(std::span<const FrameTiming>(frames));

    CHECK(stats.sampleCount == 64);
    CHECK(stats.minMs == doctest::Approx(16.0f));
    CHECK(stats.maxMs == doctest::Approx(20.0f));
}
