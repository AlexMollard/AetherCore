// Transient heap planning: which render-graph resources are allowed to share memory.
//
// The correctness case matters more than the packing case here. Overlapping the memory of
// two resources that are both live at once corrupts whichever one is read second, and the
// symptom is a rendering artefact somewhere far away from this file, so the "must not
// alias" tests are the ones worth pinning hardest.
#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "rendering/TransientHeapPacker.hpp"

using namespace aether;

namespace
{
	// Byte ranges [offset, offset + size) of two placements, do they touch?
	[[nodiscard]] bool RangesOverlap(const TransientHeapPlacement& a, std::uint64_t aSize, const TransientHeapPlacement& b, std::uint64_t bSize)
	{
		return a.offset < b.offset + bSize && b.offset < a.offset + aSize;
	}

	constexpr std::uint64_t kMiB = 1024ull * 1024ull;
} // namespace

TEST_CASE("PlanTransientHeap reuses the memory of a resource whose lifetime has ended")
{
	// A is live over passes 0..2, B only starts at pass 3. Nothing can observe A after
	// pass 2, so B is entitled to the same bytes.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 64 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 2, .aliasable = true},
	        {.size = 64 * kMiB, .alignment = 4096, .firstPass = 3, .lastPass = 5, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	REQUIRE(plan.placements.size() == 2);
	CHECK(plan.placements[0].offset == plan.placements[1].offset);
	CHECK(plan.placements[0].bucket == plan.placements[1].bucket);
	CHECK(plan.placements[0].aliased);
	CHECK(plan.placements[1].aliased);
	CHECK(plan.bucketCount == 1);

	// One bucket, so the heap costs one resource rather than two.
	CHECK(plan.totalSize == 64 * kMiB);
	CHECK(plan.standaloneSize == 128 * kMiB);
}

TEST_CASE("PlanTransientHeap never overlaps two resources that are live at the same time")
{
	// Both are live across pass 3, so they must occupy disjoint byte ranges.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 8 * kMiB, .alignment = 4096, .firstPass = 1, .lastPass = 4, .aliasable = true},
	        {.size = 8 * kMiB, .alignment = 4096, .firstPass = 3, .lastPass = 7, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	REQUIRE(plan.placements.size() == 2);
	CHECK_FALSE(RangesOverlap(plan.placements[0], requests[0].size, plan.placements[1], requests[1].size));
	CHECK(plan.placements[0].bucket != plan.placements[1].bucket);
	CHECK_FALSE(plan.placements[0].aliased);
	CHECK_FALSE(plan.placements[1].aliased);
	CHECK(plan.totalSize == 16 * kMiB);
}

TEST_CASE("PlanTransientHeap treats a single shared pass as an overlap")
{
	// Touching at exactly one pass still means both are live inside that pass.
	const std::vector<TransientHeapRequest> requests{
	        {.size = kMiB, .alignment = 256, .firstPass = 0, .lastPass = 4, .aliasable = true},
	        {.size = kMiB, .alignment = 256, .firstPass = 4, .lastPass = 9, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK_FALSE(RangesOverlap(plan.placements[0], requests[0].size, plan.placements[1], requests[1].size));
	CHECK(plan.bucketCount == 2);
}

TEST_CASE("PlanTransientHeap keeps a resource off the pool when its first access reads")
{
	// B's lifetime is disjoint from A's, but B is not aliasable: its first access reads
	// contents produced somewhere the graph cannot see, so it must own its memory.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 1, .aliasable = true},
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 5, .lastPass = 6, .aliasable = false},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK_FALSE(RangesOverlap(plan.placements[0], requests[0].size, plan.placements[1], requests[1].size));
	CHECK(plan.placements[0].bucket != plan.placements[1].bucket);
	CHECK_FALSE(plan.placements[1].aliased);
}

TEST_CASE("PlanTransientHeap will not move an aliasable resource on top of a read-first one")
{
	// The non-aliasable resource is the larger of the two, so it is placed first and opens
	// bucket 0. The aliasable one has a disjoint lifetime and would otherwise slot straight
	// in underneath it - but the read-first resource still expects its own bytes to survive.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 32 * kMiB, .alignment = 4096, .firstPass = 6, .lastPass = 9, .aliasable = false},
	        {.size = 8 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 2, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK_FALSE(RangesOverlap(plan.placements[0], requests[0].size, plan.placements[1], requests[1].size));
	CHECK(plan.placements[0].bucket != plan.placements[1].bucket);
	CHECK_FALSE(plan.placements[0].aliased);
	CHECK(plan.bucketCount == 2);
}

TEST_CASE("PlanTransientHeap never merges two non-aliasable resources")
{
	const std::vector<TransientHeapRequest> requests{
	        {.size = 2 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 1, .aliasable = false},
	        {.size = 2 * kMiB, .alignment = 4096, .firstPass = 5, .lastPass = 6, .aliasable = false},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK(plan.bucketCount == 2);
	CHECK(plan.totalSize == 4 * kMiB);
}

TEST_CASE("PlanTransientHeap chains several disjoint lifetimes through one bucket")
{
	// Three consecutive scratch targets: the heap should cost the largest of them.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 16 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 1, .aliasable = true},
	        {.size = 64 * kMiB, .alignment = 4096, .firstPass = 2, .lastPass = 3, .aliasable = true},
	        {.size = 32 * kMiB, .alignment = 4096, .firstPass = 4, .lastPass = 5, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK(plan.bucketCount == 1);
	CHECK(plan.totalSize == 64 * kMiB);
	CHECK(plan.standaloneSize == 112 * kMiB);
	CHECK(plan.placements[0].offset == plan.placements[1].offset);
	CHECK(plan.placements[1].offset == plan.placements[2].offset);
}

TEST_CASE("PlanTransientHeap honours every occupant's alignment")
{
	const std::vector<TransientHeapRequest> requests{
	        {.size = 100, .alignment = 64, .firstPass = 0, .lastPass = 1, .aliasable = true},
	        {.size = 200, .alignment = 4096, .firstPass = 3, .lastPass = 4, .aliasable = true},
	        {.size = 300, .alignment = 256, .firstPass = 0, .lastPass = 9, .aliasable = true},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	for (std::size_t i = 0; i < requests.size(); ++i)
	{
		CHECK(plan.placements[i].offset % requests[i].alignment == 0);
	}
	CHECK(plan.maxAlignment == 4096);
}

TEST_CASE("PlanTransientHeap gives every live resource a distinct range when nothing is disjoint")
{
	// Everything spans the whole frame: the plan must degrade to one bucket per resource
	// and no two ranges may touch.
	std::vector<TransientHeapRequest> requests;
	for (std::uint32_t i = 0; i < 6; ++i)
	{
		requests.push_back({.size = (i + 1) * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 10, .aliasable = true});
	}

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	CHECK(plan.bucketCount == 6);
	for (std::size_t a = 0; a < requests.size(); ++a)
	{
		for (std::size_t b = a + 1; b < requests.size(); ++b)
		{
			CHECK_FALSE(RangesOverlap(plan.placements[a], requests[a].size, plan.placements[b], requests[b].size));
		}
	}
}

TEST_CASE("PlanTransientHeap produces the same plan for the same requests")
{
	// The offsets are handed to live GPU images, so a plan that drifted between frames
	// would move memory out from under them.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 2, .aliasable = true},
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 3, .lastPass = 5, .aliasable = true},
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 1, .lastPass = 6, .aliasable = true},
	        {.size = 9 * kMiB, .alignment = 4096, .firstPass = 7, .lastPass = 8, .aliasable = true},
	};

	const TransientHeapPlan first = PlanTransientHeap(requests);
	const TransientHeapPlan second = PlanTransientHeap(requests);

	REQUIRE(first.placements.size() == second.placements.size());
	CHECK(first.totalSize == second.totalSize);
	for (std::size_t i = 0; i < first.placements.size(); ++i)
	{
		CHECK(first.placements[i].offset == second.placements[i].offset);
		CHECK(first.placements[i].bucket == second.placements[i].bucket);
	}
}

TEST_CASE("PlanTransientHeap keeps every pairwise overlap disjoint in a mixed graph")
{
	// A realistic mix: some scratch, some frame-long targets, some read-first resources.
	const std::vector<TransientHeapRequest> requests{
	        {.size = 128 * kMiB, .alignment = 65536, .firstPass = 11, .lastPass = 14, .aliasable = true},
	        {.size = 128 * kMiB, .alignment = 65536, .firstPass = 12, .lastPass = 13, .aliasable = true},
	        {.size = 64 * kMiB, .alignment = 65536, .firstPass = 10, .lastPass = 10, .aliasable = true},
	        {.size = 30 * kMiB, .alignment = 4096, .firstPass = 5, .lastPass = 37, .aliasable = true},
	        {.size = 4 * kMiB, .alignment = 4096, .firstPass = 21, .lastPass = 22, .aliasable = true},
	        {.size = 16 * kMiB, .alignment = 4096, .firstPass = 42, .lastPass = 43, .aliasable = false},
	};

	const TransientHeapPlan plan = PlanTransientHeap(requests);

	for (std::size_t a = 0; a < requests.size(); ++a)
	{
		for (std::size_t b = a + 1; b < requests.size(); ++b)
		{
			const bool livesTogether = requests[a].firstPass <= requests[b].lastPass && requests[b].firstPass <= requests[a].lastPass;
			const bool shareable = requests[a].aliasable && requests[b].aliasable;
			if (livesTogether || !shareable)
			{
				CHECK_FALSE(RangesOverlap(plan.placements[a], requests[a].size, plan.placements[b], requests[b].size));
			}
		}
	}

	// The 64 MiB atlas depth (pass 10) fits under the 128 MiB blur buffer (passes 11-14).
	CHECK(plan.placements[2].aliased);
	CHECK(plan.totalSize < plan.standaloneSize);
}

TEST_CASE("PlanTransientHeap handles the empty and zero-size cases")
{
	CHECK(PlanTransientHeap({}).totalSize == 0);

	const std::vector<TransientHeapRequest> requests{
	        {.size = 0, .alignment = 4096, .firstPass = 0, .lastPass = 1, .aliasable = true},
	        {.size = kMiB, .alignment = 4096, .firstPass = 0, .lastPass = 1, .aliasable = true},
	};
	const TransientHeapPlan plan = PlanTransientHeap(requests);
	CHECK(plan.totalSize == kMiB);
	CHECK(plan.bucketCount == 1);
}
