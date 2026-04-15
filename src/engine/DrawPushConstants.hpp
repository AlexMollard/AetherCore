#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	// The single push constant block for all standard draw calls.
	// Laid out to match the shader's [[vk::push_constant]] PushData struct.
	//
	//   offset  0 : mat4     model          (64 bytes) — per-object world transform
	//   offset 64 : uint64   frameAddr      ( 8 bytes) — BDA of FrameConstants
	//   offset 72 : uint32   materialIndex  ( 4 bytes) — index into MaterialBuffer
	//   offset 76 : uint32   _pad0          ( 4 bytes)
	//   offset 80 : uint64   skinBufferAddr ( 8 bytes) — BDA of joint matrix palette; 0 = not skinned
	//
	// Total: 88 bytes (within the 128-byte minimum Vulkan guarantee).
	struct DrawPushConstants
	{
		glm::mat4       model         { 1.0f };
		VkDeviceAddress frameAddr     = 0;
		std::uint32_t   materialIndex = 0xFFFFFFFFu; // kNoMaterial → vertex-colour fallback
		std::uint32_t   _pad0         = 0;
		VkDeviceAddress skinBufferAddr = 0;          // 0 = not skinned
	};

	static_assert(sizeof(DrawPushConstants) == 88,
		"DrawPushConstants layout changed — update gltf_mesh.slang.");
}
