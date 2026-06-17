#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	class World;

	// Bake all pendingExternalAnims on every SkinnedMeshComponent spawned
	// under entityId into their respective AnimationDatabase GPU buffers.
	// After this call the clip indices returned by add_animation() are
	// fully valid database clip indices.
	//
	// Clips with rootLocked=true have their root bone translation channels
	// stripped during compilation (prevents root motion like walking in place).
	void CompileAnimations(World& world, std::uint32_t entityId, gpu::CommandPool uploadPool);
} // namespace aether
