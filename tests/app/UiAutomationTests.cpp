#include <doctest/doctest.h>

#include "imgui/UiAutomation.hpp"

using namespace aether::app;

TEST_CASE("UiAutomation FindItem resolves by window+label and computes centre")
{
    UiAutomation& a = UiAutomation::Get();
    a.RecordItemAdd(1u, 10.0f, 20.0f, 100.0f, 40.0f, "Lighting");
    a.RecordItemInfo(1u, "Light gizmos");
    a.RecordItemAdd(2u, 0.0f, 0.0f, 50.0f, 50.0f, "Other");
    a.RecordItemInfo(2u, "Light gizmos");
    a.BeginFrameSwap(); // publish building -> snapshot

    std::string err;
    const auto hit = a.FindItem("Lighting", "Light gizmos", err);
    REQUIRE(hit.has_value());
    CHECK(hit->x + hit->w / 2.0f == doctest::Approx(60.0f));
    CHECK(hit->y + hit->h / 2.0f == doctest::Approx(40.0f));

    const auto ambiguous = a.FindItem("", "Light gizmos", err);
    CHECK_FALSE(ambiguous.has_value());
    CHECK_FALSE(err.empty());

    const auto missing = a.FindItem("Lighting", "Nope", err);
    CHECK_FALSE(missing.has_value());

    a.BeginFrameSwap(); // clear so this test leaves no residue for others
}
