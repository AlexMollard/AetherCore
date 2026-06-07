#pragma once

#include <cstdint>

#include "vulkan/volk.hpp"

namespace aether
{
	class World;

	// Set the command pool used by CompileAnimations to upload GPU data.
	// Must be called once during app init (main thread only) before any
	// compile_animations daScript call.
	void SetAnimationCompilePool(VkCommandPool pool);

	// Bake all pendingExternalAnims on every SkinnedMeshComponent spawned
	// under entityId into their respective AnimationDatabase GPU buffers.
	// After this call the clip indices returned by add_animation() are
	// fully valid database clip indices.
	void CompileAnimations(World& world, std::uint32_t entityId);
} // namespace aether
