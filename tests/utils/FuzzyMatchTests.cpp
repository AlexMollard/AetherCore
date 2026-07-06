#include <doctest/doctest.h>

#include "utils/FuzzyMatch.hpp"

using namespace aether;

TEST_CASE("FuzzyMatch: empty needle matches everything with score 0") {
    const auto s = FuzzyMatch("", "anything");
    REQUIRE(s.has_value());
    CHECK(*s == 0);
}

TEST_CASE("FuzzyMatch: subsequence matches, non-subsequence does not") {
    CHECK(FuzzyMatch("insp", "Inspector").has_value());
    CHECK(FuzzyMatch("vp", "Viewport").has_value());              // scattered subsequence
    CHECK_FALSE(FuzzyMatch("xyz", "Inspector").has_value());
    CHECK_FALSE(FuzzyMatch("inspector!", "Inspector").has_value()); // needle longer than a full match
}

TEST_CASE("FuzzyMatch: is case-insensitive") {
    CHECK(FuzzyMatch("INSP", "inspector").has_value());
    CHECK(FuzzyMatch("insp", "INSPECTOR").has_value());
}

TEST_CASE("FuzzyMatch: prefix/consecutive outranks scattered") {
    const auto prefix = FuzzyMatch("sp", "Spawn");
    const auto scattered = FuzzyMatch("sp", "Post Processing");
    REQUIRE(prefix.has_value());
    REQUIRE(scattered.has_value());
    CHECK(*prefix > *scattered);
}
