#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	// Per-frame constant data written once to a GPU buffer before any draws.
	// Accessed via Buffer Device Address pushed per draw call.
	//
	// Layout (320 bytes):
	//   offset   0 : mat4     viewProj               (64)
	//   offset  64 : mat4     view                   (64)
	//   offset 128 : mat4     proj                   (64)
	//   offset 192 : uint64   materialBufferAddr      ( 8)  BDA of MaterialBuffer
	//   offset 200 : uint64   _pad0                   ( 8)
	//   offset 208 : vec4     sunDirectionIntensity   (16)  xyz=world dir, w=intensity
	//   offset 224 : vec4     ambientColor            (16)  rgb=ambient lighting term, a=unused
	//   offset 240 : vec4     cameraWorldPos          (16)  xyz=camera position, w=1
	//   offset 256 : vec4     sunColor                (16)  rgb=sun light color, a=unused
	//   offset 272 : vec4     skyHorizonColor         (16)  rgb=sky horizon tint, a=unused
	//   offset 288 : vec4     skyZenithColor          (16)  rgb=sky zenith tint, a=unused
	//   offset 304 : vec4     skyVoidColor            (16)  rgb=below-horizon void tint, a=unused
	//   offset 320 : uvec4    tiledLightGridInfo      (16)  x=tilePx, y=tilesX, z=tilesY, w=lightCount
	//   offset 336 : uvec4    tiledLightBufferOffsets (16)  x=lightBase, y=headerBase, z=indexBase, w=unused
	//   Total: 352 bytes
	struct FrameConstants
	{
		glm::mat4       viewProj              { 1.0f };
		glm::mat4       view                  { 1.0f };
		glm::mat4       proj                  { 1.0f };
		VkDeviceAddress materialBufferAddr    = 0;
		std::uint64_t   _pad0                 = 0;
		glm::vec4       sunDirectionIntensity { 0.577f, 0.577f, 0.577f, 3.0f };
		glm::vec4       ambientColor          { 0.03f, 0.04f, 0.06f, 1.0f };
		glm::vec4       cameraWorldPos        { 0.0f, 0.0f, 0.0f, 1.0f };
		glm::vec4       sunColor              { 1.0f, 0.96f, 0.90f, 1.0f };
		glm::vec4       skyHorizonColor       { 0.34f, 0.52f, 0.82f, 1.0f };
		glm::vec4       skyZenithColor        { 0.08f, 0.19f, 0.45f, 1.0f };
		glm::vec4       skyVoidColor          { 0.001f, 0.002f, 0.005f, 1.0f };
		glm::uvec4      tiledLightGridInfo    { 0u, 0u, 0u, 0u };
		glm::uvec4      tiledLightBufferOffsets{ 0u, 0u, 0u, 0u };
	};

	static_assert(sizeof(FrameConstants) == 352,
		"FrameConstants layout changed — update the Slang structs in gltf_mesh.slang and skybox.slang.");
}
