// Transient heap planning: which render-graph resources are allowed to share memory.
//
// The correctness case matters more than the packing case here. Overlapping the memory of
// two resources that are both live at once corrupts whichever one is read second, and the
// symptom is a rendering artefact somewhere far away from this file, so the "must not
// alias" tests are the ones worth pinning hardest.
#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <string>
#include <string_view>
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

// ---------------------------------------------------------------------------------
// Heap eligibility: which slots are even offered to the planner.
//
// A slot the planner never sees is a slot the overlap rule never applies to, so the two
// legitimate skip reasons are pinned individually. The reason that used to be here and is
// gone - "this slot holds a bindless descriptor" - is pinned by its absence: the predicate
// has no bindless input, so re-introducing the exemption cannot be done quietly.
// ---------------------------------------------------------------------------------

TEST_CASE("ClaimsHeapSpace lets a live sized slot into the plan")
{
	const TransientLifetime live{.firstPass = 3, .lastPass = 9, .live = true, .discardsOnFirstUse = true};
	CHECK(ClaimsHeapSpace(31457280, live));

	// A read-first slot still claims space - it just will not be laid on top of anybody.
	const TransientLifetime readFirst{.firstPass = 3, .lastPass = 9, .live = true, .discardsOnFirstUse = false};
	CHECK(ClaimsHeapSpace(31457280, readFirst));
}

TEST_CASE("ClaimsHeapSpace keeps a slot no compiled pass touches out of the plan")
{
	const TransientLifetime dead{.firstPass = 0, .lastPass = 0, .live = false, .discardsOnFirstUse = true};
	CHECK_FALSE(ClaimsHeapSpace(31457280, dead));
}

TEST_CASE("ClaimsHeapSpace keeps a slot with no memory requirement out of the plan")
{
	const TransientLifetime live{.firstPass = 3, .lastPass = 9, .live = true, .discardsOnFirstUse = true};
	CHECK_FALSE(ClaimsHeapSpace(0, live));
}

// ---------------------------------------------------------------------------------
// The migrated editor graph, at its real sizes and compiled-pass intervals.
//
// These are the resources that moved from service ownership to the graph. Every one of
// them holds a bindless descriptor slot, which is precisely what used to keep them off the
// heap, so this is the case that has never been planned before. The shadow cascades,
// Scene.Depth and HdrColor are all live across the shadow block at once; overlapping any
// pair of them corrupts a shadow lookup or the depth prepass, and the artefact shows up
// nowhere near this file.
// ---------------------------------------------------------------------------------

namespace
{
	struct NamedRequest
	{
		const char* name;
		TransientHeapRequest request;
	};

	// Compiled-pass intervals measured from the 44-pass editor graph.
	const std::vector<NamedRequest> kMigratedGraph{
	        {"ShadowService.Depth_C0", {.size = 64 * kMiB, .alignment = 65536, .firstPass = 6, .lastPass = 17, .aliasable = true}},
	        {"ShadowService.Depth_C1", {.size = 16 * kMiB, .alignment = 65536, .firstPass = 7, .lastPass = 17, .aliasable = true}},
	        {"ShadowService.Depth_C2", {.size = 16 * kMiB, .alignment = 65536, .firstPass = 8, .lastPass = 17, .aliasable = true}},
	        {"LocalShadow.AtlasDepth", {.size = 64 * kMiB, .alignment = 65536, .firstPass = 10, .lastPass = 10, .aliasable = true}},
	        {"Scene.Depth", {.size = 15 * kMiB, .alignment = 65536, .firstPass = 4, .lastPass = 17, .aliasable = true}},
	        {"PostProcess.HdrColor", {.size = 30 * kMiB, .alignment = 65536, .firstPass = 5, .lastPass = 37, .aliasable = true}},
	        {"PostProcess.LdrColor", {.size = 15 * kMiB, .alignment = 65536, .firstPass = 34, .lastPass = 35, .aliasable = true}},
	        {"GTAO.Raw", {.size = 1 * kMiB, .alignment = 65536, .firstPass = 15, .lastPass = 16, .aliasable = true}},
	        {"GTAO.Denoised", {.size = 1 * kMiB, .alignment = 65536, .firstPass = 16, .lastPass = 17, .aliasable = true}},
	};

	[[nodiscard]] std::vector<TransientHeapRequest> RequestsOf(const std::vector<NamedRequest>& named)
	{
		std::vector<TransientHeapRequest> out;
		out.reserve(named.size());
		for (const NamedRequest& n: named)
		{
			out.push_back(n.request);
		}
		return out;
	}

	[[nodiscard]] std::size_t IndexOf(const std::vector<NamedRequest>& named, std::string_view name)
	{
		for (std::size_t i = 0; i < named.size(); ++i)
		{
			if (name == named[i].name)
			{
				return i;
			}
		}
		FAIL("no such request: " << name);
		return 0;
	}
} // namespace

TEST_CASE("PlanTransientHeap never overlaps two migrated bindless targets that are live together")
{
	// The correctness case, and the one this whole migration turns on. Being bindless buys
	// no exemption: if two of these share bytes while both are live, a shadow lookup or the
	// tonemap input reads somebody else's pixels.
	const std::vector<TransientHeapRequest> requests = RequestsOf(kMigratedGraph);
	const TransientHeapPlan plan = PlanTransientHeap(requests);

	REQUIRE(plan.placements.size() == requests.size());

	for (std::size_t a = 0; a < requests.size(); ++a)
	{
		for (std::size_t b = a + 1; b < requests.size(); ++b)
		{
			const bool livesTogether = requests[a].firstPass <= requests[b].lastPass && requests[b].firstPass <= requests[a].lastPass;
			if (livesTogether)
			{
				INFO("overlapping pair: " << std::string{kMigratedGraph[a].name} << " and " << std::string{kMigratedGraph[b].name});
				CHECK_FALSE(RangesOverlap(plan.placements[a], requests[a].size, plan.placements[b], requests[b].size));
			}
		}
	}
}

TEST_CASE("PlanTransientHeap gives the shadow block and the scene depth distinct ranges")
{
	// Spelled out rather than derived, because the pairwise sweep above would stay green if
	// the intervals themselves drifted. Cascade 0 (p6-17), Scene.Depth (p4-17) and HdrColor
	// (p5-37) are live simultaneously across the whole shadow and GTAO block.
	const std::vector<TransientHeapRequest> requests = RequestsOf(kMigratedGraph);
	const TransientHeapPlan plan = PlanTransientHeap(requests);

	const std::size_t c0 = IndexOf(kMigratedGraph, "ShadowService.Depth_C0");
	const std::size_t depth = IndexOf(kMigratedGraph, "Scene.Depth");
	const std::size_t hdr = IndexOf(kMigratedGraph, "PostProcess.HdrColor");
	const std::size_t atlasDepth = IndexOf(kMigratedGraph, "LocalShadow.AtlasDepth");

	CHECK_FALSE(RangesOverlap(plan.placements[c0], 64 * kMiB, plan.placements[depth], 15 * kMiB));
	CHECK_FALSE(RangesOverlap(plan.placements[c0], 64 * kMiB, plan.placements[hdr], 30 * kMiB));
	CHECK_FALSE(RangesOverlap(plan.placements[depth], 15 * kMiB, plan.placements[hdr], 30 * kMiB));
	// The local shadow atlas depth is only live at pass 10, inside cascade 0's span.
	CHECK_FALSE(RangesOverlap(plan.placements[atlasDepth], 64 * kMiB, plan.placements[c0], 64 * kMiB));
}

TEST_CASE("PlanTransientHeap lays the tonemap output on a finished shadow cascade")
{
	// The pooling case, and the reason the migration is worth doing at all. LdrColor
	// (p34-35) starts seventeen passes after cascade 0 (p6-17) is finished with, so the
	// 64 MiB range serves both. Before the migration LdrColor held a private 15 MiB
	// allocation for the life of the process because it was bindless.
	const std::vector<TransientHeapRequest> requests = RequestsOf(kMigratedGraph);
	const TransientHeapPlan plan = PlanTransientHeap(requests);

	const std::size_t ldr = IndexOf(kMigratedGraph, "PostProcess.LdrColor");
	const std::size_t c0 = IndexOf(kMigratedGraph, "ShadowService.Depth_C0");
	const std::size_t gtaoRaw = IndexOf(kMigratedGraph, "GTAO.Raw");
	const std::size_t atlasDepth = IndexOf(kMigratedGraph, "LocalShadow.AtlasDepth");

	CHECK(plan.placements[ldr].bucket == plan.placements[c0].bucket);
	CHECK(plan.placements[ldr].aliased);
	CHECK(plan.placements[c0].aliased);

	// GTAO's raw target (p15-16) lands on the local shadow atlas depth (p10 only).
	CHECK(plan.placements[gtaoRaw].bucket == plan.placements[atlasDepth].bucket);

	// Literal, not derived from the plan: nine resources costing 222 MiB standalone fit in
	// 206 MiB once the disjoint ones share.
	CHECK(plan.standaloneSize == 232783872);
	CHECK(plan.totalSize == 216006656);
	CHECK(plan.bucketCount == 7);
}
