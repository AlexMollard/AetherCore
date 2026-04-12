#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace meow
{
	// The single push constant block for all standard draw calls.
	// Laid out to match the shader's [[vk::push_constant]] struct.
	//
	//   offset  0 : mat4     model          (64 bytes) — per-object world transform
	//   offset 64 : u64      frameAddr      ( 8 bytes) — BDA pointer to FrameConstants
	//   offset 72 : uint32_t albedoSlot     ( 4 bytes) — bindless sampled-image index
	//   offset 76 : uint32_t _pad0          ( 4 bytes) — explicit padding
	//
	// Total: 80 bytes (well within the 128-byte minimum Vulkan guarantee).
	struct DrawPushConstants
	{
		glm::mat4       model{ 1.0f };
		VkDeviceAddress frameAddr  = 0;        // VkDeviceAddress = uint64_t
		std::uint32_t   albedoSlot = 0xFFFFFFFFu; // 0xFFFFFFFF → vertex colour fallback
		std::uint32_t   _pad0      = 0;
	};

	static_assert(sizeof(DrawPushConstants) == 80,
		"DrawPushConstants layout changed — update shaders.");
}
