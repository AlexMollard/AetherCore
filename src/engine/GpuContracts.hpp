#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include "volk.hpp"

namespace aether
{
	// Per-instance payload consumed by the standard mesh shader path.
	// Keep in sync with shaders/include/RenderContracts.slangh.
	struct DrawInstanceData
	{
		glm::mat4 model{ 1.0f };
		std::uint32_t materialIndex = 0xFFFFFFFFu;
		std::uint32_t skinPaletteOffset = 0;
		std::uint32_t skinJointCount = 0;
		std::uint32_t _pad0 = 0;
		glm::vec4 worldBoundingSphere{};
	};

	static_assert(sizeof(DrawInstanceData) == 96, "DrawInstanceData layout changed - update shaders/include/RenderContracts.slangh.");
	static_assert(offsetof(DrawInstanceData, model) == 0);
	static_assert(offsetof(DrawInstanceData, materialIndex) == 64);
	static_assert(offsetof(DrawInstanceData, skinPaletteOffset) == 68);
	static_assert(offsetof(DrawInstanceData, skinJointCount) == 72);
	static_assert(offsetof(DrawInstanceData, _pad0) == 76);
	static_assert(offsetof(DrawInstanceData, worldBoundingSphere) == 80);

	struct DrawPushConstants
	{
		VkDeviceAddress frameAddr = 0;
		VkDeviceAddress instanceDataAddr = 0;
		VkDeviceAddress skinPaletteAddr = 0;
	};

	static_assert(sizeof(DrawPushConstants) == 24, "DrawPushConstants layout changed - update shaders/include/RenderContracts.slangh.");
	static_assert(offsetof(DrawPushConstants, frameAddr) == 0);
	static_assert(offsetof(DrawPushConstants, instanceDataAddr) == 8);
	static_assert(offsetof(DrawPushConstants, skinPaletteAddr) == 16);

	struct CullDrawInput
	{
		std::uint32_t indexCount = 0;
		std::uint32_t instanceCount = 1;
		std::uint32_t firstIndex = 0;
		std::int32_t vertexOffset = 0;
		std::uint32_t firstInstance = 0;
		std::uint32_t batchIndex = 0;
		std::uint32_t _pad[2]{};
	};

	static_assert(sizeof(CullDrawInput) == 32, "CullDrawInput layout changed - update shaders/include/RenderContracts.slangh.");
	static_assert(offsetof(CullDrawInput, indexCount) == 0);
	static_assert(offsetof(CullDrawInput, instanceCount) == 4);
	static_assert(offsetof(CullDrawInput, firstIndex) == 8);
	static_assert(offsetof(CullDrawInput, vertexOffset) == 12);
	static_assert(offsetof(CullDrawInput, firstInstance) == 16);
	static_assert(offsetof(CullDrawInput, batchIndex) == 20);
	static_assert(offsetof(CullDrawInput, _pad) == 24);

	struct CullBatch
	{
		std::uint32_t inputStart = 0;
		std::uint32_t drawCount = 0;
		std::uint32_t outputStart = 0;
		std::uint32_t _pad = 0;
	};

	static_assert(sizeof(CullBatch) == 16, "CullBatch layout changed - update shaders/include/RenderContracts.slangh.");
	static_assert(offsetof(CullBatch, inputStart) == 0);
	static_assert(offsetof(CullBatch, drawCount) == 4);
	static_assert(offsetof(CullBatch, outputStart) == 8);
	static_assert(offsetof(CullBatch, _pad) == 12);

	struct CullPushConstants
	{
		VkDeviceAddress frameAddr = 0;
		VkDeviceAddress instanceDataAddr = 0;
		VkDeviceAddress inputCmdAddr = 0;
		VkDeviceAddress outputCmdAddr = 0;
		VkDeviceAddress batchDescAddr = 0;
		VkDeviceAddress batchCountAddr = 0;
		std::uint32_t totalDrawCount = 0;
		std::uint32_t debugFlags = 0;
		std::uint32_t _pad0 = 0;
		std::uint32_t _pad1 = 0;
	};

	static constexpr std::uint32_t kCullDebugForceVisibleBit = 1u << 0;

	static_assert(sizeof(CullPushConstants) == 64, "CullPushConstants layout changed - update shaders/include/RenderContracts.slangh.");
	static_assert(offsetof(CullPushConstants, frameAddr) == 0);
	static_assert(offsetof(CullPushConstants, instanceDataAddr) == 8);
	static_assert(offsetof(CullPushConstants, inputCmdAddr) == 16);
	static_assert(offsetof(CullPushConstants, outputCmdAddr) == 24);
	static_assert(offsetof(CullPushConstants, batchDescAddr) == 32);
	static_assert(offsetof(CullPushConstants, batchCountAddr) == 40);
	static_assert(offsetof(CullPushConstants, totalDrawCount) == 48);
	static_assert(offsetof(CullPushConstants, debugFlags) == 52);
	static_assert(offsetof(CullPushConstants, _pad0) == 56);
	static_assert(offsetof(CullPushConstants, _pad1) == 60);

	struct SkinCopyJob
	{
		VkDeviceAddress srcPaletteAddr = 0;
		VkDeviceAddress sampledPosesAddr = 0;
		std::uint32_t dstPaletteOffset = 0;
		std::uint32_t jointCount = 0;
		std::uint32_t skinIndex = 0;
		std::uint32_t nodeCount = 0;
		float blendAlpha = 1.0f;
		std::uint32_t useSampledPoses = 0;
		std::uint32_t _pad0 = 0;
		std::uint32_t _pad1 = 0;
	};

	static_assert(sizeof(SkinCopyJob) == 48, "SkinCopyJob layout changed - update shaders/include/AnimationContracts.slangh.");
	static_assert(offsetof(SkinCopyJob, srcPaletteAddr) == 0);
	static_assert(offsetof(SkinCopyJob, sampledPosesAddr) == 8);
	static_assert(offsetof(SkinCopyJob, dstPaletteOffset) == 16);
	static_assert(offsetof(SkinCopyJob, jointCount) == 20);
	static_assert(offsetof(SkinCopyJob, skinIndex) == 24);
	static_assert(offsetof(SkinCopyJob, nodeCount) == 28);
	static_assert(offsetof(SkinCopyJob, blendAlpha) == 32);
	static_assert(offsetof(SkinCopyJob, useSampledPoses) == 36);
	static_assert(offsetof(SkinCopyJob, _pad0) == 40);
	static_assert(offsetof(SkinCopyJob, _pad1) == 44);

	struct AnimatorSampleJob
	{
		std::uint32_t animClipIndex = 0;
		float animTime = 0.0f;
		std::uint32_t nodePoseOffset = 0;
		std::uint32_t nodeCount = 0;
	};

	static_assert(sizeof(AnimatorSampleJob) == 16, "AnimatorSampleJob layout changed - update shaders/include/AnimationContracts.slangh.");
	static_assert(offsetof(AnimatorSampleJob, animClipIndex) == 0);
	static_assert(offsetof(AnimatorSampleJob, animTime) == 4);
	static_assert(offsetof(AnimatorSampleJob, nodePoseOffset) == 8);
	static_assert(offsetof(AnimatorSampleJob, nodeCount) == 12);

	struct SampledNodePose
	{
		glm::vec4 translation{ 0.0f, 0.0f, 0.0f, 0.0f };
		glm::vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
		glm::vec4 scale{ 1.0f, 1.0f, 1.0f, 0.0f };
	};

	static_assert(sizeof(SampledNodePose) == 48, "SampledNodePose layout changed - update shaders/include/AnimationContracts.slangh.");
	static_assert(offsetof(SampledNodePose, translation) == 0);
	static_assert(offsetof(SampledNodePose, rotation) == 16);
	static_assert(offsetof(SampledNodePose, scale) == 32);

	struct SkinPalettePush
	{
		VkDeviceAddress jobsAddr = 0;
		VkDeviceAddress dstPaletteAddr = 0;
		VkDeviceAddress prevPaletteAddr = 0;
		VkDeviceAddress nodeParentsAddr = 0;
		VkDeviceAddress skinMetasAddr = 0;
		VkDeviceAddress skinJointsAddr = 0;
		VkDeviceAddress skinInverseBindsAddr = 0;
		std::uint32_t jobCount = 0;
		std::uint32_t _pad0 = 0;
		std::uint32_t _pad1 = 0;
		std::uint32_t _pad2 = 0;
	};

	static_assert(sizeof(SkinPalettePush) == 72, "SkinPalettePush layout changed - update shaders/include/AnimationContracts.slangh.");
	static_assert(offsetof(SkinPalettePush, jobsAddr) == 0);
	static_assert(offsetof(SkinPalettePush, dstPaletteAddr) == 8);
	static_assert(offsetof(SkinPalettePush, prevPaletteAddr) == 16);
	static_assert(offsetof(SkinPalettePush, nodeParentsAddr) == 24);
	static_assert(offsetof(SkinPalettePush, skinMetasAddr) == 32);
	static_assert(offsetof(SkinPalettePush, skinJointsAddr) == 40);
	static_assert(offsetof(SkinPalettePush, skinInverseBindsAddr) == 48);
	static_assert(offsetof(SkinPalettePush, jobCount) == 56);
	static_assert(offsetof(SkinPalettePush, _pad0) == 60);
	static_assert(offsetof(SkinPalettePush, _pad1) == 64);
	static_assert(offsetof(SkinPalettePush, _pad2) == 68);

	struct AnimationSamplePush
	{
		VkDeviceAddress animDbClipsAddr = 0;
		VkDeviceAddress animDbChannelsAddr = 0;
		VkDeviceAddress animDbTimesAddr = 0;
		VkDeviceAddress animDbValuesAddr = 0;
		VkDeviceAddress bindTranslationsAddr = 0;
		VkDeviceAddress bindRotationsAddr = 0;
		VkDeviceAddress bindScalesAddr = 0;
		VkDeviceAddress animatorJobsAddr = 0;
		VkDeviceAddress sampledPosesAddr = 0;
		std::uint32_t jobCount = 0;
		std::uint32_t clipCount = 0;
	};

	static_assert(sizeof(AnimationSamplePush) == 80, "AnimationSamplePush layout changed - update shaders/include/AnimationContracts.slangh.");
	static_assert(offsetof(AnimationSamplePush, animDbClipsAddr) == 0);
	static_assert(offsetof(AnimationSamplePush, animDbChannelsAddr) == 8);
	static_assert(offsetof(AnimationSamplePush, animDbTimesAddr) == 16);
	static_assert(offsetof(AnimationSamplePush, animDbValuesAddr) == 24);
	static_assert(offsetof(AnimationSamplePush, bindTranslationsAddr) == 32);
	static_assert(offsetof(AnimationSamplePush, bindRotationsAddr) == 40);
	static_assert(offsetof(AnimationSamplePush, bindScalesAddr) == 48);
	static_assert(offsetof(AnimationSamplePush, animatorJobsAddr) == 56);
	static_assert(offsetof(AnimationSamplePush, sampledPosesAddr) == 64);
	static_assert(offsetof(AnimationSamplePush, jobCount) == 72);
	static_assert(offsetof(AnimationSamplePush, clipCount) == 76);
} // namespace aether
