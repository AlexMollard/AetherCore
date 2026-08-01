#pragma once

#include <cstddef>
#include <optional>

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	// Remembers which tag allocated each live block.
	//
	// This exists because `operator delete` receives a pointer and nothing else. Attributing
	// the free to the FREEING thread's current tag would be wrong in the common case - a mesh
	// buffer built under `Mesh` and released during untagged teardown would subtract from
	// `Unknown` and leave `Mesh` permanently inflated, so the panel would show a leak that is
	// not there. Sizes come back from `mi_usable_size`, but the tag has nowhere to live except
	// here.
	//
	// Sharded so concurrent allocations on different threads rarely contend on the same lock;
	// pointers spread evenly across shards because they are distinct addresses.
	//
	// EVERY method must be called from inside a TrackerReentryGuard. The table's own storage
	// allocates, and those allocations route back through the replaced operator new; the guard
	// is what stops that recursing.
	class TagTable
	{
	public:
		void Insert(const void* pointer, MemTag tag) noexcept;

		// Removes and returns the tag, because a block is freed exactly once - leaving it
		// behind would grow the table for the life of the process.
		[[nodiscard]] std::optional<MemTag> Take(const void* pointer) noexcept;

		void Clear() noexcept;

		[[nodiscard]] std::size_t Size() const noexcept;

		static constexpr std::size_t kShards = 64;
	};

	// Must only be reached from inside a TrackerReentryGuard: the first call constructs the
	// shards, and that construction allocates.
	[[nodiscard]] TagTable& GlobalTagTable() noexcept;
} // namespace aether::memory
