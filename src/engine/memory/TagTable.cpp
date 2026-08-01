#include "memory/TagTable.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>

#include "memory/TrackerReentry.hpp"

namespace aether::memory
{
	namespace
	{
		struct Shard
		{
			mutable std::mutex mutex;
			std::unordered_map<const void*, MemTag> entries;
		};

		// Function-local rather than a namespace-scope object: the shards contain mutexes and
		// hash maps, so constructing them allocates, and a namespace-scope constructor would
		// run during static initialization in an order this cannot control. Deferring to
		// first use is safe precisely because every caller is already inside a
		// TrackerReentryGuard, so the construction's own allocations are not tracked and
		// cannot recurse.
		//
		// IMMORTAL: constructed in place and never destroyed. `operator delete` keeps being
		// called throughout static destruction and after it - every object with static storage
		// duration in the process frees something on the way out - so a table that destroyed
		// itself would be read after its own lifetime ended. Letting the storage outlive the
		// process is the point, not an oversight; it is reclaimed by exit like any other page.
		// (Destroying it segfaulted the test binary at shutdown after every test had passed.)
		Shard* Shards() noexcept
		{
			alignas(Shard) static std::byte storage[sizeof(Shard) * TagTable::kShards];
			static Shard* const shards = []
			{
				auto* first = reinterpret_cast<Shard*>(storage);
				for (std::size_t i = 0; i < TagTable::kShards; ++i)
				{
					::new (static_cast<void*>(first + i)) Shard();
				}
				return first;
			}();
			return shards;
		}

		[[nodiscard]] std::size_t ShardOf(const void* pointer) noexcept
		{
			// Shift past the low bits: allocations are aligned, so those bits are largely
			// constant and hashing on them would pile every pointer into a few shards.
			const auto value = reinterpret_cast<std::uintptr_t>(pointer);
			return (value >> 4) % TagTable::kShards;
		}
	} // namespace

	void TagTable::Insert(const void* pointer, const MemTag tag) noexcept
	{
		if (pointer == nullptr)
		{
			return;
		}
		Shard& shard = Shards()[ShardOf(pointer)];
		const std::lock_guard lock(shard.mutex);
		shard.entries[pointer] = tag;
	}

	std::optional<MemTag> TagTable::Take(const void* pointer) noexcept
	{
		if (pointer == nullptr)
		{
			return std::nullopt;
		}
		Shard& shard = Shards()[ShardOf(pointer)];
		const std::lock_guard lock(shard.mutex);
		const auto found = shard.entries.find(pointer);
		if (found == shard.entries.end())
		{
			// Legitimately common: anything allocated before tracking was switched on, or
			// while it was disabled, has no entry. The caller must treat this as "do not
			// subtract" rather than as an error, or disabling and re-enabling tracking would
			// drive counters negative.
			return std::nullopt;
		}
		const MemTag tag = found->second;
		shard.entries.erase(found);
		return tag;
	}

	void TagTable::Clear() noexcept
	{
		for (std::size_t i = 0; i < kShards; ++i)
		{
			Shard& shard = Shards()[i];
			const std::lock_guard lock(shard.mutex);
			shard.entries.clear();
		}
	}

	std::size_t TagTable::Size() const noexcept
	{
		std::size_t total = 0;
		for (std::size_t i = 0; i < kShards; ++i)
		{
			const Shard& shard = Shards()[i];
			const std::lock_guard lock(shard.mutex);
			total += shard.entries.size();
		}
		return total;
	}

	TagTable& GlobalTagTable() noexcept
	{
		static TagTable table;
		return table;
	}
} // namespace aether::memory
