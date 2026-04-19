#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include "volk.hpp"

namespace aether
{
	// Per-frame constant data written once to a GPU buffer before any draws.
	// Accessed via Buffer Device Address pushed per draw call.
	//
	inline constexpr std::uint32_t kShadowCascadeCount = 3u;

	// Layout (624 bytes):
	//   offset   0 : mat4     viewProj               (64)
	//   offset  64 : mat4     view                   (64)
	//   offset 128 : mat4     proj                   (64)
	//   offset 192 : uint64   materialBufferAddr      ( 8)  BDA of MaterialBuffer
	//   offset 200 : uint64   _pad0                   ( 8)
	//   offset 208 : vec4     sunDirectionIntensity   (16)  xyz=world dir,
	//   w=intensity offset 224 : vec4     ambientColor            (16)  rgb=ambient
	//   lighting term, a=unused offset 240 : vec4     cameraWorldPos          (16)
	//   xyz=camera position, w=1 offset 256 : vec4     sunColor                (16)
	//   rgb=sun light color, a=unused offset 272 : vec4     skyHorizonColor (16)
	//   rgb=sky horizon tint, a=unused offset 288 : vec4     skyZenithColor (16)
	//   rgb=sky zenith tint, a=unused offset 304 : vec4     skyVoidColor (16)
	//   rgb=below-horizon void tint, a=unused offset 320 : uvec4 tiledLightGridInfo
	//   (16)  x=tilePx, y=tilesX, z=tilesY, w=lightCount offset 336 : uvec4
	//   tiledLightBufferOffsets (16)  x=lightBase, y=headerBase, z=indexBase,
	//   w=unused offset 352 : mat4[3]  shadowViewProjCascades (192)
	//   light clip-space transforms for CSM cascades offset 544 : vec4
	//   shadowCascadeSplits (16) xyz=split far distances in view-space units
	//   offset 560 : vec4 shadowParams (16) x=depthBias, y=normalBias,
	//   z=strength, w=pcfRadiusTexels offset 576 : uvec4[3] shadowCascadeInfo
	//   (48) each: x=bindlessSlot (or 0xFFFFFFFF), y=width, z=height, w=unused
	//   Total: 624 bytes
	struct FrameConstants
	{
		glm::mat4 viewProj{ 1.0f };
		glm::mat4 view{ 1.0f };
		glm::mat4 proj{ 1.0f };
		VkDeviceAddress materialBufferAddr = 0;
		std::uint64_t _pad0 = 0;
		glm::vec4 sunDirectionIntensity{ 0.577f, 0.577f, 0.577f, 3.0f };
		glm::vec4 ambientColor{ 0.03f, 0.04f, 0.06f, 1.0f };
		glm::vec4 cameraWorldPos{ 0.0f, 0.0f, 0.0f, 1.0f };
		glm::vec4 sunColor{ 1.0f, 0.96f, 0.90f, 1.0f };
		glm::vec4 skyHorizonColor{ 0.34f, 0.52f, 0.82f, 1.0f };
		glm::vec4 skyZenithColor{ 0.08f, 0.19f, 0.45f, 1.0f };
		glm::vec4 skyVoidColor{ 0.001f, 0.002f, 0.005f, 1.0f };
		glm::uvec4 tiledLightGridInfo{ 0u, 0u, 0u, 0u };
		glm::uvec4 tiledLightBufferOffsets{ 0u, 0u, 0u, 0u };
		std::array<glm::mat4, kShadowCascadeCount> shadowViewProjCascades{ glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
		glm::vec4 shadowCascadeSplits{ 24.0f, 80.0f, 220.0f, 0.0f };
		glm::vec4 shadowParams{ 0.0008f, 0.0012f, 1.0f, 1.5f };
		std::array<glm::uvec4, kShadowCascadeCount> shadowCascadeInfo{
			glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
			glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
			glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
		};
	};

	static_assert(sizeof(FrameConstants) == 624,
	        "FrameConstants layout changed — update the Slang structs in "
	        "gltf_mesh.slang and skybox.slang.");
	static_assert(offsetof(FrameConstants, viewProj) == 0);
	static_assert(offsetof(FrameConstants, view) == 64);
	static_assert(offsetof(FrameConstants, proj) == 128);
	static_assert(offsetof(FrameConstants, materialBufferAddr) == 192);
	static_assert(offsetof(FrameConstants, _pad0) == 200);
	static_assert(offsetof(FrameConstants, sunDirectionIntensity) == 208);
	static_assert(offsetof(FrameConstants, ambientColor) == 224);
	static_assert(offsetof(FrameConstants, cameraWorldPos) == 240);
	static_assert(offsetof(FrameConstants, sunColor) == 256);
	static_assert(offsetof(FrameConstants, skyHorizonColor) == 272);
	static_assert(offsetof(FrameConstants, skyZenithColor) == 288);
	static_assert(offsetof(FrameConstants, skyVoidColor) == 304);
	static_assert(offsetof(FrameConstants, tiledLightGridInfo) == 320);
	static_assert(offsetof(FrameConstants, tiledLightBufferOffsets) == 336);
	static_assert(offsetof(FrameConstants, shadowViewProjCascades) == 352);
	static_assert(offsetof(FrameConstants, shadowCascadeSplits) == 544);
	static_assert(offsetof(FrameConstants, shadowParams) == 560);
	static_assert(offsetof(FrameConstants, shadowCascadeInfo) == 576);
} // namespace aether
