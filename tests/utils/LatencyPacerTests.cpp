#include <doctest/doctest.h>

#include "utils/LatencyPacer.hpp"

using aether::LatencyPacer;

namespace
{
	constexpr float kInterval = 16.67f; // 60 Hz
}

// The slack is a constant, as it is in Unreal (rhi.SyncSlackMS). It does not learn, decay or
// ratchet - all three of which the previous adaptive version did, and which let it saturate
// at the whole interval and switch pacing off permanently.
TEST_CASE("The slack is whatever it was set to") {
    LatencyPacer pacer;
    pacer.SetSlackMs(3.0f);

    CHECK(pacer.ReserveMs(kInterval) == doctest::Approx(3.0f));
}

TEST_CASE("The default matches Unreal's rhi.SyncSlackMS") {
    LatencyPacer pacer;

    CHECK(pacer.ReserveMs(100.0f) == doctest::Approx(LatencyPacer::kDefaultSlackMs));
}

// Zero slack is legitimate - it is what practitioners run with r.GTSyncType 2 - and must
// latch as late as the pacer allows rather than being treated as "off".
TEST_CASE("Zero slack is honoured rather than ignored") {
    LatencyPacer pacer;
    pacer.SetSlackMs(0.0f);

    CHECK(pacer.ReserveMs(kInterval) == doctest::Approx(0.0f));
}

TEST_CASE("A negative slack is floored at zero") {
    LatencyPacer pacer;
    pacer.SetSlackMs(-5.0f);

    CHECK(pacer.ReserveMs(kInterval) == doctest::Approx(0.0f));
}

// A slack of a whole interval puts the latch point on the PREVIOUS flip, which is always in
// the past, so no frame would ever be paced. This is the failure the adaptive version fell
// into; a constant must not be able to reach it either.
TEST_CASE("Slack is held short of the interval so the latch point stays in the future") {
    LatencyPacer pacer;
    pacer.SetSlackMs(1000.0f);

    const float reserve = pacer.ReserveMs(kInterval);

    CHECK(reserve < kInterval);
    CHECK(reserve == doctest::Approx(kInterval * 0.9f));
}

// A shorter interval means less room, and the cap has to follow it rather than a constant.
TEST_CASE("The cap scales with the display period") {
    LatencyPacer pacer;
    pacer.SetSlackMs(1000.0f);

    CHECK(pacer.ReserveMs(8.33f) == doctest::Approx(8.33f * 0.9f)); // 120 Hz
    CHECK(pacer.ReserveMs(33.33f) == doctest::Approx(33.33f * 0.9f)); // 30 Hz
}

TEST_CASE("A zero or negative interval reserves nothing rather than going negative") {
    LatencyPacer pacer;
    pacer.SetSlackMs(5.0f);

    CHECK(pacer.ReserveMs(0.0f) == doctest::Approx(0.0f));
    CHECK(pacer.ReserveMs(-1.0f) == doctest::Approx(0.0f));
}
