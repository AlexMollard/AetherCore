#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <numbers>

namespace aether
{
	// Per-frame constant data written once to a GPU buffer before any draws.
	// Accessed via Buffer Device Address pushed per draw call.
	//
	// Usage Notes:
	// - skyVoidColor: Active feature used by skybox.slang:208 for void/space rendering, set by DayNightSystem
	// - shadowCascadeInfo: Populated by ShadowService for CSM (3 cascades)
	// - tiledLightGridInfo/tiledLightBufferOffsets: Used by tiled_light_cull.slang for local lights
	// - shadowViewProjCascades: Directional light cascade matrices (ShadowService)
	// - shadowAtlasSlot: Bindless slot for local shadow atlas (LocalShadowService)
	//
	// Sync with shaders/include/FrameConstants.slangh - update both when modifying.
	//
	inline constexpr std::uint32_t kShadowCascadeCount = 3u;

	// Layout (640 bytes):
	//   offset   0 : mat4     viewProj               (64)  Combined view-projection matrix
	//   offset  64 : mat4     view                   (64)  View matrix
	//   offset 128 : mat4     proj                   (64)  Projection matrix
	//   offset 192 : uint64   materialBufferAddr      ( 8)  BDA of MaterialBuffer
	//   offset 200 : float    elapsedTime             ( 4)  Frame elapsed time in seconds
	//   offset 204 : uint32   _pad0                   ( 4)  Alignment padding
	//   offset 208 : vec4     sunDirectionIntensity   (16)  xyz=world-space direction, w=intensity (directional light)
	//   offset 224 : vec4     ambientColor            (16)  rgb=ambient term, a=unused
	//   offset 240 : vec4     cameraWorldPos          (16)  xyz=camera position, w=1
	//   offset 256 : vec4     sunColor                (16)  rgb=sun light color, a=unused
	//   offset 272 : vec4     skyHorizonColor         (16)  rgb=sky horizon gradient, a=unused (skybox.slang)
	//   offset 288 : vec4     skyZenithColor          (16)  rgb=sky zenith gradient, a=unused (skybox.slang)
	//   offset 304 : vec4     skyVoidColor            (16)  rgb=void/space tint, a=unused (skybox.slang:208, DayNightSystem)
	//   offset 320 : uvec4    tiledLightGridInfo      (16)  x=tilePx, y=tilesX, z=tilesY, w=lightCount (tiled lighting)
	//   offset 336 : uvec4    tiledLightBufferOffsets (16)  x=lightBase, y=headerBase, z=indexBase, w=unused
	//   offset 352 : mat4[3]  shadowViewProjCascades (192)  Light clip transforms for CSM cascades (ShadowService)
	//   offset 544 : vec4     shadowCascadeSplits     (16)  xyz=split far distances for 3 cascades
	//   offset 560 : vec4     shadowParams            (16)  x=depthBias, y=normalBias, z=strength, w=pcfRadiusTexels
	//   offset 576 : uvec4[3] shadowCascadeInfo       (48)  Each: x=bindlessSlot, y=width, z=height, w=unused
	//   offset 624 : uint     shadowAtlasSlot         ( 4)  Bindless slot for VSM atlas (0xFFFFFFFF = none, LocalShadowService)
	//   offset 628 : uint     shadowLightCount        ( 4)  Number of active shadow-casting lights
	//   offset 632 : uint64   shadowLightDataAddr     ( 8)  BDA to ShadowLightData[]
	//   Total: 640 bytes
	struct FrameConstants
	{
		glm::mat4 viewProj{1.0f};                                                                                                           // offset 0
		glm::mat4 view{1.0f};                                                                                                               // offset 64
		glm::mat4 proj{1.0f};                                                                                                               // offset 128
		std::uint64_t materialBufferAddr = 0;                                                                                               // offset 192
		float elapsedTime = 0.0f;                                                                                                           // offset 200
		std::uint32_t _pad0 = 0;                                                                                                            // offset 204
		glm::vec4 sunDirectionIntensity{std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, 3.0f}; // offset 208, directional light
		glm::vec4 ambientColor{0.03f, 0.04f, 0.06f, 1.0f};                                                                                  // offset 224
		glm::vec4 cameraWorldPos{0.0f, 0.0f, 0.0f, 1.0f};                                                                                   // offset 240
		glm::vec4 sunColor{1.0f, 0.96f, 0.90f, 1.0f};                                                                                       // offset 256
		glm::vec4 skyHorizonColor{0.34f, 0.52f, 0.82f, 1.0f};                                                                               // offset 272, skybox.slang
		glm::vec4 skyZenithColor{0.08f, 0.19f, 0.45f, 1.0f};                                                                                // offset 288, skybox.slang
		glm::vec4 skyVoidColor{0.001f, 0.002f, 0.005f, 1.0f};                                                                               // offset 304, skybox.slang:208
		glm::uvec4 tiledLightGridInfo{0u, 0u, 0u, 0u};                                                                                      // offset 320, tiled lighting
		glm::uvec4 tiledLightBufferOffsets{0u, 0u, 0u, 0u};                                                                                 // offset 336, tiled lighting
		std::array<glm::mat4, kShadowCascadeCount> shadowViewProjCascades{glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};               // offset 352
		glm::vec4 shadowCascadeSplits{24.0f, 80.0f, 220.0f, 0.0f};                                                                          // offset 544, CSM splits
		glm::vec4 shadowParams{0.0008f, 0.0012f, 1.0f, 1.5f};                                                                               // offset 560, shadow params
		std::array<glm::uvec4, kShadowCascadeCount> shadowCascadeInfo{
		        // offset 576, CSM bindless slots
		        glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
		        glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
		        glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u),
		};
		std::uint32_t shadowAtlasSlot = 0xFFFFFFFFu; // offset 624, local shadows
		std::uint32_t shadowLightCount = 0;          // offset 628
		std::uint64_t shadowLightDataAddr = 0;       // offset 632
	};

	static_assert(sizeof(FrameConstants) == 640,
	        "FrameConstants layout changed - update the Slang structs in "
	        "gltf_mesh.slang and skybox.slang.");
	static_assert(offsetof(FrameConstants, viewProj) == 0);
	static_assert(offsetof(FrameConstants, view) == 64);
	static_assert(offsetof(FrameConstants, proj) == 128);
	static_assert(offsetof(FrameConstants, materialBufferAddr) == 192);
	static_assert(offsetof(FrameConstants, elapsedTime) == 200);
	static_assert(offsetof(FrameConstants, _pad0) == 204);
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
	static_assert(offsetof(FrameConstants, shadowAtlasSlot) == 624);
	static_assert(offsetof(FrameConstants, shadowLightCount) == 628);
	static_assert(offsetof(FrameConstants, shadowLightDataAddr) == 632);
	static_assert(sizeof(FrameConstants) == 640);
} // namespace aether
