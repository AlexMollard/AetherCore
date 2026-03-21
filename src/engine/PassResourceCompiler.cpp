#include "PassResourceCompiler.hpp"

#include <algorithm>
#include <numeric>

namespace meow
{
	namespace
	{
		template <typename TRequest>
		[[nodiscard]] std::vector<std::uint32_t> BuildMaterializationOrder(const std::vector<TRequest>& requests)
		{
			std::vector<std::uint32_t> order(requests.size());
			std::iota(order.begin(), order.end(), 0);
			std::stable_sort(order.begin(), order.end(), [&](const std::uint32_t lhs, const std::uint32_t rhs) {
				const auto& a = requests[lhs].lifetime;
				const auto& b = requests[rhs].lifetime;
				if (a.firstPass != b.firstPass)
				{
					return a.firstPass < b.firstPass;
				}

				const auto aLength = a.lastPass - a.firstPass;
				const auto bLength = b.lastPass - b.firstPass;
				return aLength < bLength;
				});
			return order;
		}
	}

	CompiledResourceSet PassResourceCompiler::Compile(
		ResourcePool& pool,
		const std::vector<BufferLifetimeRequest>& bufferRequests,
		const std::vector<ImageLifetimeRequest>& imageRequests,
		const ResourcePool::BufferFactory& bufferFactory,
		const ResourcePool::ImageFactory& imageFactory)
	{
		CompiledResourceSet out{};
		out.buffers.reserve(bufferRequests.size());
		for (const auto& request : bufferRequests)
		{
			out.buffers.push_back(pool.CreateVirtualBuffer(request.desc, request.lifetime, request.transient));
		}

		out.images.reserve(imageRequests.size());
		for (const auto& request : imageRequests)
		{
			out.images.push_back(pool.CreateVirtualImage(request.desc, request.lifetime, request.transient));
		}

		const auto bufferOrder = BuildMaterializationOrder(bufferRequests);
		for (const auto idx : bufferOrder)
		{
			pool.MaterializeBuffer(out.buffers[idx], bufferFactory);
		}

		const auto imageOrder = BuildMaterializationOrder(imageRequests);
		for (const auto idx : imageOrder)
		{
			pool.MaterializeImage(out.images[idx], imageFactory);
		}

		return out;
	}
}
