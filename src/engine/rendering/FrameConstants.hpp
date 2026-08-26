#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <numbers>

namespace aether
{
	inline constexpr std::uint32_t kShadowCascadeCount = 3u;

	// Layout (816 bytes):
	struct FrameConstants
	{
		glm::mat4 viewProj{1.0f};
		glm::mat4 view{1.0f};
		glm::mat4 proj{1.0f};
		std::uint64_t materialBufferAddr = 0;
		float elapsedTime = 0.0f;
		std::uint32_t _pad0 = 0;
		glm::vec4 sunDirectionIntensity{std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, 3.0f};
		glm::vec4 ambientColor{0.03f, 0.04f, 0.06f, 1.0f};
		glm::vec4 cameraWorldPos{0.0f, 0.0f, 0.0f, 1.0f};
		glm::vec4 sunColor{1.0f, 0.96f, 0.90f, 1.0f};
		glm::vec4 skyHorizonColor{0.34f, 0.52f, 0.82f, 1.0f};
		glm::vec4 skyZenithColor{0.08f, 0.19f, 0.45f, 1.0f};
		glm::vec4 skyVoidColor{0.001f, 0.002f, 0.005f, 1.0f};
		glm::uvec4 tiledLightGridInfo{0u, 0u, 0u, 0u};
		glm::uvec4 tiledLightBufferOffsets{0u, 0u, 0u, 0u};
		std::array<glm::mat4, kShadowCascadeCount> shadowViewProjCascades{glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
		glm::vec4 shadowCascadeSplits{24.0f, 80.0f, 220.0f, 0.0f};
		glm::vec4 shadowParams{1.0f, 1.5f, 1.0f, 0.0f}; // x=depthBiasTexels, y=normalOffsetTexels, z=strength
		std::uint32_t shadowLightCount = 0;
		std::uint32_t _padShadowAlign = 0; // offset 580, alignment before uint64
		std::uint64_t shadowLightDataAddr = 0;
		std::array<glm::vec4, 6> frustumPlanes{
		        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
		        glm::vec4(-1.0f, 0.0f, 0.0f, 1.0f),
		        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
		        glm::vec4(0.0f, -1.0f, 0.0f, 1.0f),
		        glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),
		        glm::vec4(0.0f, 0.0f, -1.0f, 1.0f),
		};
		glm::mat4 invViewProj{1.0f};
		std::uint64_t resourceTableAddr = 0;
		std::uint64_t effectParamBufferAddr = 0;
		// Per-cascade shadow bias, derived on the CPU from each cascade's real texel
		// footprint and ortho depth range (ShadowService::BuildFrameShadowData). xyz =
		// cascade 0..2, w unused.
		glm::vec4 shadowCascadeDepthBias{0.0f};   // constant bias in NDC depth units
		glm::vec4 shadowCascadeNormalOffset{0.0f}; // normal-offset distance in world units
		// Penumbra UV width per unit of NDC depth separation between blocker and
		// receiver, i.e. the sun's angular size expressed in this cascade's own
		// shadow-map space. xyz = cascade 0..2, w unused.
		glm::vec4 shadowCascadePenumbraScale{0.0f};

		void RefreshDerived()
		{
			invViewProj = glm::inverse(viewProj);

			const glm::vec4 row0{viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]};
			const glm::vec4 row1{viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]};
			const glm::vec4 row2{viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]};
			const glm::vec4 row3{viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]};

			frustumPlanes[0] = NormalizePlane(row3 + row0);
			frustumPlanes[1] = NormalizePlane(row3 - row0);
			frustumPlanes[2] = NormalizePlane(row3 + row1);
			frustumPlanes[3] = NormalizePlane(row3 - row1);
			frustumPlanes[4] = NormalizePlane(row2);
			frustumPlanes[5] = NormalizePlane(row3 - row2);
		}

		[[nodiscard]] static glm::vec4 NormalizePlane(glm::vec4 plane)
		{
			const float len = glm::length(glm::vec3(plane));
			return (len > 1e-6f) ? (plane / len) : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		}
	};

	static_assert(sizeof(FrameConstants) == 816, "FrameConstants layout changed - update shaders/include/FrameConstants.slangh.");
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
	static_assert(offsetof(FrameConstants, shadowLightCount) == 576);
	static_assert(offsetof(FrameConstants, _padShadowAlign) == 580);
	static_assert(offsetof(FrameConstants, shadowLightDataAddr) == 584);
	static_assert(offsetof(FrameConstants, frustumPlanes) == 592);
	static_assert(offsetof(FrameConstants, invViewProj) == 688);
	static_assert(offsetof(FrameConstants, resourceTableAddr) == 752);
	static_assert(offsetof(FrameConstants, effectParamBufferAddr) == 760);
	static_assert(offsetof(FrameConstants, shadowCascadeDepthBias) == 768);
	static_assert(offsetof(FrameConstants, shadowCascadeNormalOffset) == 784);
	static_assert(offsetof(FrameConstants, shadowCascadePenumbraScale) == 800);
	static_assert(sizeof(FrameConstants) == 816);
} // namespace aether
