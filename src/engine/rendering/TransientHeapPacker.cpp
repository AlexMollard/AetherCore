#include "rendering/TransientHeapPacker.hpp"

#include <algorithm>
#include <numeric>

namespace aether
{
	namespace
	{
		[[nodiscard]] std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
		{
			if (alignment <= 1)
			{
				return value;
			}
			return (value + alignment - 1) & ~(alignment - 1);
		}

		struct Bucket
		{
			std::uint64_t size = 0;
			std::uint64_t alignment = 1;
			std::uint64_t offset = 0;
			std::vector<std::uint32_t> occupants;
		};

		[[nodiscard]] bool Overlaps(const TransientHeapRequest& a, const TransientHeapRequest& b)
		{
			return a.firstPass <= b.lastPass && b.firstPass <= a.lastPass;
		}
	} // namespace

	TransientHeapPlan PlanTransientHeap(std::span<const TransientHeapRequest> requests)
	{
		TransientHeapPlan plan;
		plan.placements.resize(requests.size());
		if (requests.empty())
		{
			return plan;
		}

		std::vector<std::uint32_t> order(requests.size());
		std::iota(order.begin(), order.end(), 0u);
		// Largest first, then by index: two resources of equal size must always land in the
		// same order or the plan would shuffle between frames and invalidate live offsets.
		std::ranges::sort(order,
		        [&](const std::uint32_t a, const std::uint32_t b)
		        {
			        if (requests[a].size != requests[b].size)
			        {
				        return requests[a].size > requests[b].size;
			        }
			        return a < b;
		        });

		std::vector<Bucket> buckets;
		for (const std::uint32_t idx: order)
		{
			const TransientHeapRequest& req = requests[idx];
			if (req.size == 0)
			{
				continue;
			}

			std::size_t target = buckets.size();
			if (req.aliasable)
			{
				for (std::size_t b = 0; b < buckets.size(); ++b)
				{
					const bool fits = std::ranges::none_of(buckets[b].occupants,
					        [&](const std::uint32_t occupant) { return !requests[occupant].aliasable || Overlaps(requests[occupant], req); });
					if (fits)
					{
						target = b;
						break;
					}
				}
			}

			if (target == buckets.size())
			{
				buckets.emplace_back();
			}

			Bucket& bucket = buckets[target];
			bucket.size = std::max(bucket.size, req.size);
			bucket.alignment = std::max(bucket.alignment, std::max<std::uint64_t>(req.alignment, 1));
			bucket.occupants.push_back(idx);
		}

		std::uint64_t cursor = 0;
		for (Bucket& bucket: buckets)
		{
			bucket.offset = AlignUp(cursor, bucket.alignment);
			cursor = bucket.offset + bucket.size;
			plan.maxAlignment = std::max(plan.maxAlignment, bucket.alignment);
		}
		plan.totalSize = cursor;
		plan.bucketCount = static_cast<std::uint32_t>(buckets.size());

		for (std::uint32_t b = 0; b < buckets.size(); ++b)
		{
			const Bucket& bucket = buckets[b];
			for (const std::uint32_t idx: bucket.occupants)
			{
				plan.placements[idx] = TransientHeapPlacement{
				        .offset = bucket.offset,
				        .bucket = b,
				        .aliased = bucket.occupants.size() > 1,
				};
			}
		}

		std::uint64_t standalone = 0;
		for (const TransientHeapRequest& req: requests)
		{
			if (req.size == 0)
			{
				continue;
			}
			standalone = AlignUp(standalone, std::max<std::uint64_t>(req.alignment, 1)) + req.size;
		}
		plan.standaloneSize = standalone;

		return plan;
	}
} // namespace aether
