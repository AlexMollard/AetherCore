#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	// Per-frame constant data written once to a GPU buffer before any draws.
	// Accessed via Buffer Device Address pushed per draw call.
	//
	// Layout (256 bytes):
	//   offset   0 : mat4     viewProj               (64)
	//   offset  64 : mat4     view                   (64)
	//   offset 128 : mat4     proj                   (64)
	//   offset 192 : uint64   materialBufferAddr      ( 8)  BDA of MaterialBuffer
	//   offset 200 : uint64   _pad0                   ( 8)
	//   offset 208 : vec4     sunDirectionIntensity   (16)  xyz=world dir, w=intensity
	//   offset 224 : vec4     ambientColor            (16)  rgb=sky color, a=unused
	//   Total: 240 bytes
	struct FrameConstants
	{
		glm::mat4       viewProj              { 1.0f };
		glm::mat4       view                  { 1.0f };
		glm::mat4       proj                  { 1.0f };
		VkDeviceAddress materialBufferAddr    = 0;
		std::uint64_t   _pad0                 = 0;
		glm::vec4       sunDirectionIntensity { 0.577f, 0.577f, 0.577f, 3.0f };
		glm::vec4       ambientColor          { 0.03f, 0.04f, 0.06f, 1.0f };
	};

	static_assert(sizeof(FrameConstants) == 240,
		"FrameConstants layout changed — update the Slang struct in gltf_mesh.slang.");
}
