#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	class World;

	// Set the command pool used by CompileAnimations to upload GPU data.
	// Must be called once during app init (main thread only) before any
	// compile_animations daScript call.
	void SetAnimationCompilePool(gpu::CommandPool pool);

	// Bake all pendingExternalAnims on every SkinnedMeshComponent spawned
	// under entityId into their respective AnimationDatabase GPU buffers.
	// After this call the clip indices returned by add_animation() are
	// fully valid database clip indices.
	//
	// Clips with rootLocked=true have their root bone translation channels
	// stripped during compilation (see add_animation / load_external_animation).
	void CompileAnimations(World& world, std::uint32_t entityId);
} // namespace aether
