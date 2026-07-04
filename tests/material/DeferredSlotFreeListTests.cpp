#include <doctest/doctest.h>
#include "material/DeferredSlotFreeList.hpp"

using namespace aether;

TEST_CASE("Allocate hands out distinct slots and reports exhaustion") {
    DeferredSlotFreeList a;
    a.Reset(3);
    CHECK(a.FreeCount() == 3);

    const std::uint32_t s0 = a.Allocate();
    const std::uint32_t s1 = a.Allocate();
    const std::uint32_t s2 = a.Allocate();
    CHECK(s0 != s1);
    CHECK(s1 != s2);
    CHECK(s0 != s2);
    CHECK(a.Allocate() == DeferredSlotFreeList::kInvalidSlot); // exhausted
}

TEST_CASE("A freed slot is not reused until the in-flight window elapses") {
    DeferredSlotFreeList a;
    a.Reset(1); // single slot forces reuse-or-nothing

    const std::uint32_t slot = a.Allocate();
    CHECK(slot != DeferredSlotFreeList::kInvalidSlot);

    a.Free(slot); // freed on frame 0 -> reusable at frame kReuseDelayFrames
    CHECK(a.PendingCount() == 1);

    // Still within the window: the slot must NOT come back yet.
    for (std::uint64_t f = 1; f < DeferredSlotFreeList::kReuseDelayFrames; ++f)
    {
        a.AdvanceFrame(f);
        CHECK(a.Allocate() == DeferredSlotFreeList::kInvalidSlot);
    }

    // Window elapsed: the slot returns to circulation.
    a.AdvanceFrame(DeferredSlotFreeList::kReuseDelayFrames);
    CHECK(a.PendingCount() == 0);
    CHECK(a.Allocate() == slot);
}

TEST_CASE("Multiple frees retire independently by their own free frame") {
    DeferredSlotFreeList a;
    a.Reset(2);

    const std::uint32_t s0 = a.Allocate();
    const std::uint32_t s1 = a.Allocate();

    a.Free(s0);                 // retire at kReuseDelayFrames
    a.AdvanceFrame(2);
    a.Free(s1);                 // retire at 2 + kReuseDelayFrames (later)

    a.AdvanceFrame(DeferredSlotFreeList::kReuseDelayFrames);
    // s0 matured, s1 has not.
    CHECK(a.Allocate() == s0);
    CHECK(a.Allocate() == DeferredSlotFreeList::kInvalidSlot);

    a.AdvanceFrame(2 + DeferredSlotFreeList::kReuseDelayFrames);
    CHECK(a.Allocate() == s1);
}
