#include <doctest/doctest.h>

#include <cstddef>

#include "utils/LatencyPacer.hpp"

using namespace aether;

namespace
{
	constexpr float kInterval = 16.67f; // 60 Hz

	void Settle(LatencyPacer& pacer, const float workMs, const std::size_t frames)
	{
		for (std::size_t i = 0; i < frames; ++i)
		{
			pacer.Observe(kInterval, workMs, false);
		}
	}
} // namespace

// Latency is only worth taking if it is never paid for with a dropped frame, so the pacer
// must not act on measurements it does not have yet.
TEST_CASE("The pacer reserves nothing before it has warmed up") {
    LatencyPacer pacer;

    CHECK(pacer.ReserveMs() == doctest::Approx(0.0f));
    CHECK_FALSE(pacer.IsWarm());

    pacer.Observe(kInterval, 0.2f, false);
    CHECK(pacer.ReserveMs() == doctest::Approx(0.0f));
}

// The point of the whole exercise: a loop doing 0.2 ms of work should reserve a small slice
// of a 16.67 ms interval, leaving the rest to idle before latching input.
TEST_CASE("A near-idle loop reserves only a small part of the interval") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);

    CHECK(pacer.IsWarm());
    CHECK(pacer.ReserveMs() < 3.0f);
    // Never surrender the whole interval - the OS needs slack to wake us.
    CHECK(pacer.ReserveMs() >= LatencyPacer::kFloorMs);
}

TEST_CASE("Heavier work immediately reserves more of the interval") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);
    const float idle = pacer.ReserveMs();

    pacer.Observe(kInterval, 6.0f, false);

    CHECK(pacer.ReserveMs() > idle);
    CHECK(pacer.ReserveMs() >= 6.0f); // must cover the work it just saw
}

// Rising late is what drops a frame, so the response to growth is instant.
TEST_CASE("The reserve rises in a single frame but gives ground slowly") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);

    pacer.Observe(kInterval, 8.0f, false);
    const float raised = pacer.ReserveMs();
    CHECK(raised >= 8.0f);

    pacer.Observe(kInterval, 0.2f, false); // one quiet frame must not undo it
    CHECK(pacer.ReserveMs() > raised * 0.9f);

    Settle(pacer, 0.2f, 500); // sustained quiet eventually does
    CHECK(pacer.ReserveMs() < raised * 0.5f);
}

TEST_CASE("A missed flip surrenders a large part of the interval at once") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);
    const float before = pacer.ReserveMs();

    pacer.Observe(kInterval, 0.2f, true);

    CHECK(pacer.ReserveMs() > before * 2.0f);
}

// A pathological frame must never make the pacer ask for more than exists, which would
// produce a negative idle and a busy loop.
TEST_CASE("Work larger than the interval clamps to the interval") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);

    pacer.Observe(kInterval, 500.0f, true);

    CHECK(pacer.ReserveMs() == doctest::Approx(kInterval));
}

TEST_CASE("A zero or negative interval is ignored rather than poisoning the reserve") {
    LatencyPacer pacer;
    Settle(pacer, 0.2f, 200);
    const float before = pacer.ReserveMs();

    pacer.Observe(0.0f, 5.0f, false);
    pacer.Observe(-1.0f, 5.0f, true);

    CHECK(pacer.ReserveMs() == doctest::Approx(before));
}
