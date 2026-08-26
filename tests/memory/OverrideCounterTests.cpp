#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "memory/MemoryScope.hpp"
#include "memory/MemoryStats.hpp"
#include "memory/MemoryTag.hpp"
#include "memory/TrackingLevel.hpp"

using namespace aether;

namespace
{
	struct TrackingGuard
	{
		memory::TrackingLevel previous = memory::CurrentLevel();
		explicit TrackingGuard(const memory::TrackingLevel level)
		{
			memory::SetTrackingLevel(level);
		}
		~TrackingGuard()
		{
			memory::SetTrackingLevel(previous);
		}
	};

	// The standard lets a compiler delete a new/delete pair whose storage is never
	// observed, and at the optimisation level these tests are built at, clang does exactly
	// that - so the tracker has nothing to count and the test measures an allocation that
	// never happened. Writing through a volatile pointer is an observable side effect on
	// the block, which forces it to exist.
	//
	// This matters just as much for the tests that expect NO counting: without it they
	// pass whether the tracker is correct or the allocation simply vanished.
	void Touch(std::byte* block) noexcept
	{
		*static_cast<volatile std::byte*>(block) = std::byte{1};
	}
} // namespace

TEST_CASE("An allocation under a scope is counted against that tag") {
    const TrackingGuard guard(memory::TrackingLevel::Counters);
    const std::uint64_t before = memory::GlobalStats().Get(memory::MemTag::Particles).currentBytes;

    {
        AE_MEM_SCOPE(memory::MemTag::Particles);
        auto* block = new std::byte[4096];
        Touch(block);
        CHECK(memory::GlobalStats().Get(memory::MemTag::Particles).currentBytes >= before + 4096);
        delete[] block;
    }

    CHECK(memory::GlobalStats().Get(memory::MemTag::Particles).currentBytes == before);
}

// The whole reason the tag table exists. A block allocated under one tag and released under
// another must subtract from the tag that ALLOCATED it, or that tag stays inflated forever
// and the panel reports a leak that is not there.
TEST_CASE("A free attributes to the allocating tag, not the freeing one") {
    const TrackingGuard guard(memory::TrackingLevel::Counters);
    const std::uint64_t meshBefore = memory::GlobalStats().Get(memory::MemTag::Mesh).currentBytes;
    const std::uint64_t audioBefore = memory::GlobalStats().Get(memory::MemTag::Audio).currentBytes;

    std::byte* block = nullptr;
    {
        AE_MEM_SCOPE(memory::MemTag::Mesh);
        block = new std::byte[8192];
        Touch(block);
    }
    CHECK(memory::GlobalStats().Get(memory::MemTag::Mesh).currentBytes >= meshBefore + 8192);

    {
        AE_MEM_SCOPE(memory::MemTag::Audio);
        delete[] block;
    }

    CHECK(memory::GlobalStats().Get(memory::MemTag::Mesh).currentBytes == meshBefore);
    CHECK(memory::GlobalStats().Get(memory::MemTag::Audio).currentBytes == audioBefore);
}

TEST_CASE("Nothing is counted while tracking is disabled") {
    const TrackingGuard guard(memory::TrackingLevel::Disabled);
    const std::uint64_t before = memory::GlobalStats().Get(memory::MemTag::Net).currentBytes;

    {
        AE_MEM_SCOPE(memory::MemTag::Net);
        auto* block = new std::byte[2048];
        Touch(block);
        CHECK(memory::GlobalStats().Get(memory::MemTag::Net).currentBytes == before);
        delete[] block;
    }

    CHECK(memory::GlobalStats().Get(memory::MemTag::Net).currentBytes == before);
}

// A block allocated while tracking was on must still be subtracted after tracking is turned
// off, or the counters hold bytes that are long gone.
TEST_CASE("A block outlives a tracking-level change without stranding bytes") {
    const TrackingGuard guard(memory::TrackingLevel::Counters);
    const std::uint64_t before = memory::GlobalStats().Get(memory::MemTag::Tilemap).currentBytes;

    std::byte* block = nullptr;
    {
        AE_MEM_SCOPE(memory::MemTag::Tilemap);
        block = new std::byte[4096];
        Touch(block);
    }
    CHECK(memory::GlobalStats().Get(memory::MemTag::Tilemap).currentBytes >= before + 4096);

    memory::SetTrackingLevel(memory::TrackingLevel::Disabled);
    delete[] block;

    CHECK(memory::GlobalStats().Get(memory::MemTag::Tilemap).currentBytes == before);
}

// The tracker's own bookkeeping allocates, and those allocations come back through the same
// operators. Without the reentry guard this never returns at all.
TEST_CASE("Heavy tagged allocation does not recurse or deadlock") {
    const TrackingGuard guard(memory::TrackingLevel::Counters);
    AE_MEM_SCOPE(memory::MemTag::Temp);

    std::vector<std::unique_ptr<std::byte[]>> blocks;
    for (int i = 0; i < 2000; ++i)
    {
        blocks.push_back(std::make_unique<std::byte[]>(64));
    }
    CHECK(blocks.size() == 2000);
    blocks.clear();

    CHECK(memory::GlobalStats().Get(memory::MemTag::Temp).totalAllocations > 0);
}
