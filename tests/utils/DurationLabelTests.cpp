// Shared by the launcher's "3 hr ago" card subtitle and the recovery prompt's "3 hr ahead
// of the saved scene". It was the launcher's private ladder until the recovery prompt
// needed the same phrasing; these pin the strings so extracting it stayed a no-op there.
#include <doctest/doctest.h>

#include "utils/StringUtils.hpp"

using aether::utils::DurationLabel;

TEST_CASE("DurationLabel picks the largest unit that still reads as a whole number")
{
	CHECK(DurationLabel(0) == "0 sec");
	CHECK(DurationLabel(45) == "45 sec");
	CHECK(DurationLabel(59) == "59 sec");
	CHECK(DurationLabel(60) == "1 min");
	CHECK(DurationLabel(3599) == "59 min");
	CHECK(DurationLabel(3600) == "1 hr");
	CHECK(DurationLabel(60 * 60 * 24 - 1) == "23 hr");
	CHECK(DurationLabel(60 * 60 * 24) == "1 day");
	CHECK(DurationLabel(60 * 60 * 24 * 7) == "1 week");
	CHECK(DurationLabel(60LL * 60LL * 24LL * 21LL) == "3 weeks");
}

TEST_CASE("DurationLabel singularises days and weeks but never seconds, minutes or hours")
{
	// "1 days ahead of the saved scene" is the kind of thing nobody notices until it ships.
	CHECK(DurationLabel(60 * 60 * 24) == "1 day");
	CHECK(DurationLabel(60 * 60 * 24 * 2) == "2 days");
	CHECK(DurationLabel(60 * 60 * 24 * 7) == "1 week");
	CHECK(DurationLabel(60 * 60 * 24 * 14) == "2 weeks");
	// The abbreviated units read the same either way, so they carry no plural.
	CHECK(DurationLabel(1) == "1 sec");
	CHECK(DurationLabel(60) == "1 min");
	CHECK(DurationLabel(3600) == "1 hr");
}

TEST_CASE("DurationLabel treats a negative span as zero")
{
	// File timestamps can land either side of each other on a coarse clock; a recovery
	// copy reading "-3 sec ahead" would be worse than useless.
	CHECK(DurationLabel(-1) == "0 sec");
	CHECK(DurationLabel(-100000) == "0 sec");
}
