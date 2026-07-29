#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace aether
{
	// A transient resource's claim on the render graph's memory heap.
	//
	// The pass interval is inclusive and expressed in compiled-pass order: the resource
	// holds live contents from the first pass that touches it up to and including the last
	// one. Two resources may share memory only when their intervals are disjoint.
	struct TransientHeapRequest
	{
		std::uint64_t size = 0;
		std::uint64_t alignment = 1;
		std::uint32_t firstPass = 0;
		std::uint32_t lastPass = 0;
		// Sharing is only sound when the resource's first access throws away whatever the
		// previous occupant left behind - a cleared or don't-care attachment, a storage
		// write, a transfer write. A resource whose first access reads is never shared.
		bool aliasable = false;
	};

	// What the render graph knows about one transient slot after compiling the frame.
	struct TransientLifetime
	{
		std::uint32_t firstPass = 0;
		std::uint32_t lastPass = 0;
		// At least one compiled pass touches the slot this frame.
		bool live = false;
		// The slot's first access writes without reading what was there, so its memory may
		// be handed on from a resource whose lifetime has already ended.
		bool discardsOnFirstUse = false;
	};

	struct TransientHeapPlacement
	{
		std::uint64_t offset = 0;
		std::uint32_t bucket = 0;
		// True when at least one other resource shares this placement's bucket.
		bool aliased = false;
	};

	struct TransientHeapPlan
	{
		// Parallel to the request span.
		std::vector<TransientHeapPlacement> placements;
		std::uint64_t totalSize = 0;
		std::uint64_t maxAlignment = 1;
		std::uint32_t bucketCount = 0;
		// What a dedicated allocation per resource would have cost, so callers can report
		// the saving instead of guessing at it.
		std::uint64_t standaloneSize = 0;
	};

	// Packs the requests into the smallest heap that respects their lifetimes.
	//
	// Greedy by descending size: each resource takes the first bucket whose occupants all
	// have disjoint intervals, otherwise it opens a new one. Deterministic for a given set
	// of requests, which matters because the plan has to stay stable while the graph does.
	[[nodiscard]] TransientHeapPlan PlanTransientHeap(std::span<const TransientHeapRequest> requests);
} // namespace aether
