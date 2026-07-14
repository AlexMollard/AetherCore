#include <doctest/doctest.h>
#include "material/DeferredSlotFreeList.hpp"

using namespace aether;

// DeferredSlotFreeList (no dedup layer on top, spec §4.E). These tests lock in

TEST_CASE("Effect slots: two allocations are always distinct (no aliasing)") {
	DeferredSlotFreeList alloc;
	alloc.Reset(8);
	const std::uint32_t a = alloc.Allocate();
	const std::uint32_t b = alloc.Allocate();
	CHECK(a != DeferredSlotFreeList::kInvalidSlot);
	CHECK(b != DeferredSlotFreeList::kInvalidSlot);
	CHECK(a != b); // same-effect entities never collide
}

TEST_CASE("Effect slots: freed slot not reused until kReuseDelayFrames elapse") {
	DeferredSlotFreeList alloc;
	alloc.Reset(1);
	const std::uint32_t s = alloc.Allocate();
	CHECK(s == 0u);
	alloc.Free(s);
	CHECK(alloc.Allocate() == DeferredSlotFreeList::kInvalidSlot);

	for (std::uint64_t f = 1; f < DeferredSlotFreeList::kReuseDelayFrames; ++f)
	{
		alloc.AdvanceFrame(f);
		CHECK(alloc.Allocate() == DeferredSlotFreeList::kInvalidSlot);
	}
	alloc.AdvanceFrame(DeferredSlotFreeList::kReuseDelayFrames);
	CHECK(alloc.Allocate() == 0u);
}
