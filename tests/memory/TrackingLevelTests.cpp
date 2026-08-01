#include <doctest/doctest.h>

#include <string>

#include "memory/TrackingLevel.hpp"

using namespace aether::memory;

namespace
{
	// The level is process-global, so a test that changes it must put it back or it leaks
	// into every test that runs afterwards.
	struct LevelGuard
	{
		TrackingLevel previous = CurrentLevel();
		~LevelGuard()
		{
			SetTrackingLevel(previous);
		}
	};
} // namespace

// The levels are compared with `<`, so their relative order is load-bearing: inserting one
// in the middle would silently change the meaning of every LevelAtLeast call.
TEST_CASE("Levels are ordered from cheapest to most expensive") {
    CHECK(TrackingLevel::Disabled < TrackingLevel::Counters);
    CHECK(TrackingLevel::Counters < TrackingLevel::Ledger);
    CHECK(TrackingLevel::Ledger < TrackingLevel::Callstacks);
}

// EngineTests is a dev-tooling build, so the ceiling must be the top.
TEST_CASE("A dev build can reach every level") {
    CHECK(kMaxTrackingLevel == TrackingLevel::Callstacks);
}

TEST_CASE("Setting a level takes effect and LevelAtLeast follows it") {
    const LevelGuard guard;

    SetTrackingLevel(TrackingLevel::Ledger);
    CHECK(CurrentLevel() == TrackingLevel::Ledger);
    CHECK(LevelAtLeast(TrackingLevel::Counters));
    CHECK(LevelAtLeast(TrackingLevel::Ledger));
    CHECK_FALSE(LevelAtLeast(TrackingLevel::Callstacks));

    SetTrackingLevel(TrackingLevel::Disabled);
    CHECK_FALSE(LevelAtLeast(TrackingLevel::Counters));
}

// A settings file is shared across configurations. Asking for more than a build can give
// must degrade to what it can, not fail - otherwise one developer's preference breaks
// everyone else's retail build.
TEST_CASE("Requesting more than the build allows clamps to the ceiling") {
    const LevelGuard guard;

    SetTrackingLevel(TrackingLevel::Callstacks);
    CHECK(CurrentLevel() <= kMaxTrackingLevel);
    CHECK(CurrentLevel() == kMaxTrackingLevel);
}

TEST_CASE("Every level round-trips through its name") {
    for (const TrackingLevel level: {TrackingLevel::Disabled, TrackingLevel::Counters, TrackingLevel::Ledger, TrackingLevel::Callstacks})
    {
        const auto parsed = ParseTrackingLevel(ToString(level));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == level);
    }
}

// Distinguishing "unrecognised" from "Disabled" is what lets the settings layer warn about a
// typo instead of silently turning tracking off.
TEST_CASE("Unrecognised text parses to nothing rather than to Disabled") {
    CHECK_FALSE(ParseTrackingLevel("").has_value());
    CHECK_FALSE(ParseTrackingLevel("counters").has_value()); // case-sensitive by design
    CHECK_FALSE(ParseTrackingLevel("Everything").has_value());
}

TEST_CASE("An out-of-range level still yields a name") {
    CHECK(std::string(ToString(static_cast<TrackingLevel>(200))) == "Disabled");
}
