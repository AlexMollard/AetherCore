#include <doctest/doctest.h>

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

TEST_CASE("UiInputScript click plays pos, pos+down, up over three frames")
{
    UiInputScript s;
    s.QueueClick(100.0f, 50.0f, 0, false);
    CHECK(s.Busy());

    const auto f1 = s.Step(); // hover/pos established
    CHECK(CountKind(f1, SynKind::MousePos) == 1);
    CHECK(CountKind(f1, SynKind::MouseButton) == 0);

    const auto f2 = s.Step(); // button down
    REQUIRE(CountKind(f2, SynKind::MouseButton) == 1);
    CHECK(f2.back().down == true);
    CHECK(f2.back().button == 0);

    const auto f3 = s.Step(); // button up
    REQUIRE(CountKind(f3, SynKind::MouseButton) == 1);
    CHECK(f3.back().down == false);

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
