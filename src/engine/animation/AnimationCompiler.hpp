#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	class World;

	void CompileAnimations(World& world, std::uint32_t entityId, gpu::CommandPool uploadPool);
} // namespace aether
