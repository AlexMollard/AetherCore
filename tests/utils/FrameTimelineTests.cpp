#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "utils/FrameTimeline.hpp"

using namespace aether;

namespace
{
	FrameTiming MakeGameFrame(const std::uint64_t index, const float wallMs)
	{
		FrameTiming timing;
		timing.frameIndex = index;
		timing.wallMs = wallMs;
		timing.simDtMs = wallMs > 33.33f ? 33.33f : wallMs;
		timing.pacerWaitMs = 0.0f;
		timing.inFlightWaitMs = wallMs * 0.5f;
		timing.gameWorkMs = wallMs * 0.5f;
		return timing;
	}
} // namespace

TEST_CASE("A recorded game frame reads back with every phase intact") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(7, 16.0f));

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 1);
    CHECK(out[0].frameIndex == 7);
    CHECK(out[0].wallMs == doctest::Approx(16.0f));
    CHECK(out[0].inFlightWaitMs == doctest::Approx(8.0f));
    CHECK(out[0].gameWorkMs == doctest::Approx(8.0f));
    CHECK_FALSE(out[0].renderComplete);
}

TEST_CASE("The render thread completes a frame the game thread already published") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(3, 16.0f));
    timeline.RecordGameFrame(MakeGameFrame(4, 16.0f));
    // The render thread trails, so it lands on an older, already-published record.
    timeline.RecordRenderFrame(3, 1.25f, 12.5f);

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 2);
    CHECK(out[0].frameIndex == 3);
    CHECK(out[0].renderExecMs == doctest::Approx(1.25f));
    CHECK(out[0].presentWaitMs == doctest::Approx(12.5f));
    CHECK(out[0].renderComplete);
    // The newer frame is still awaiting its render-thread half.
    CHECK(out[1].frameIndex == 4);
    CHECK_FALSE(out[1].renderComplete);
}

TEST_CASE("Snapshot returns the newest frames in order after the ring wraps") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    // Oldest surviving record first, newest last, contiguous.
    CHECK(out.front().frameIndex == 10);
    CHECK(out.back().frameIndex == FrameTimeline::kCapacity + 9);
    for (std::size_t i = 1; i < count; ++i)
    {
        CHECK(out[i].frameIndex == out[i - 1].frameIndex + 1);
    }
    CHECK(timeline.FrameCount() == FrameTimeline::kCapacity + 10);
}

TEST_CASE("A render completion for an evicted frame is dropped, not misapplied") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }
    // Frame 0 was overwritten long ago; its slot now holds a much newer frame and
    // must not be corrupted by a late completion.
    timeline.RecordRenderFrame(0, 99.0f, 99.0f);

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    for (std::size_t i = 0; i < count; ++i)
    {
        CHECK(out[i].renderExecMs == doctest::Approx(0.0f));
    }
}

TEST_CASE("Snapshot into a buffer smaller than the history returns the newest that fit") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < 50; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::array<FrameTiming, 5> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 5);
    CHECK(out.front().frameIndex == 45);
    CHECK(out.back().frameIndex == 49);
}
