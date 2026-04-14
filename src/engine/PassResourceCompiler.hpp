#pragma once

#include <cstdint>
#include <vector>

#include "ResourcePool.hpp"

namespace aether
{
	struct BufferLifetimeRequest
	{
		BufferResourceDesc desc{};
		ResourceContract contract{};
	};

	struct ImageLifetimeRequest
	{
		ImageResourceDesc desc{};
		ResourceContract contract{};
	};

	struct CompiledResourceSet
	{
		std::vector<VirtualBufferHandle> buffers;
		std::vector<VirtualImageHandle> images;
	};

	class PassResourceCompiler
	{
	public:
		static CompiledResourceSet Compile(
			ResourcePool& pool,
			const std::vector<BufferLifetimeRequest>& bufferRequests,
			const std::vector<ImageLifetimeRequest>& imageRequests,
			const ResourcePool::BufferFactory& bufferFactory,
			const ResourcePool::ImageFactory& imageFactory);
	};
}
