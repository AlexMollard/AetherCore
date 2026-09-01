#include <doctest/doctest.h>

#include <vector>

#include "imgui/UiInputScript.hpp"

using namespace aether::app;

static int CountKind(const std::vector<SynEvent>& evs, SynKind k)
{
    int n = 0;
    for (const auto& e: evs)
    {
        if (e.kind == k) ++n;
    }
    return n;
}

TEST_CASE("UiInputScript click settles the cursor, then presses and releases")
{
    UiInputScript s;
    s.QueueClick(100.0f, 50.0f, 0, false);
    CHECK(s.Busy());

    // Several position-only frames first. ImGui resolves hover at the start of a frame, so a
    // press on the first frame the cursor appears somewhere new lands before the target knows
    // it is hovered; one settling frame was enough only when the cursor was already there.
    int settleFrames = 0;
    std::vector<SynEvent> frame = s.Step();
    while (CountKind(frame, SynKind::MouseButton) == 0)
    {
        CHECK(CountKind(frame, SynKind::MousePos) == 1);
        ++settleFrames;
        REQUIRE(settleFrames < 8); // a runaway would hang the loop rather than fail
        frame = s.Step();
    }
    CHECK(settleFrames >= 2);

    // The press, on the frame after the cursor has settled.
    REQUIRE(CountKind(frame, SynKind::MouseButton) == 1);
    CHECK(frame.back().down == true);
    CHECK(frame.back().button == 0);

    const auto release = s.Step();
    REQUIRE(CountKind(release, SynKind::MouseButton) == 1);
    CHECK(release.back().down == false);

    CHECK_FALSE(s.Busy());
    CHECK(s.Step().empty()); // idle, no hover held
}

TEST_CASE("UiInputScript hover holds position across frames until cleared")
{
    UiInputScript s;
    s.QueueHover(10.0f, 20.0f);
    for (int i = 0; i < 3; ++i)
    {
        const auto f = s.Step();
        REQUIRE(CountKind(f, SynKind::MousePos) == 1);
        CHECK(f.front().x == doctest::Approx(10.0f));
    }
    s.ClearHover();
    CHECK(s.Step().empty());
}

TEST_CASE("UiInputScript double click plays two button cycles")
{
    UiInputScript s;
    s.QueueClick(0.0f, 0.0f, 0, true);
    int downs = 0, ups = 0;
    while (s.Busy())
    {
        for (const auto& e: s.Step())
        {
            if (e.kind == SynKind::MouseButton) (e.down ? downs : ups)++;
        }
    }
    CHECK(downs == 2);
    CHECK(ups == 2);
}

TEST_CASE("UiInputScript key plays a down then up")
{
    UiInputScript s;
    s.QueueKey(525 /* arbitrary ImGuiKey int */);
    const auto f1 = s.Step();
    REQUIRE(CountKind(f1, SynKind::Key) == 1);
    CHECK(f1.back().down == true);
    const auto f2 = s.Step();
    REQUIRE(CountKind(f2, SynKind::Key) == 1);
    CHECK(f2.back().down == false);
    CHECK_FALSE(s.Busy());
}

TEST_CASE("UiInputScript text clears then emits a char per code point")
{
    UiInputScript s;
    s.QueueText("Hi");
    int chars = 0;
    while (s.Busy())
    {
        for (const auto& e: s.Step())
        {
            if (e.kind == SynKind::Char) ++chars;
        }
    }
    CHECK(chars == 2);
}
