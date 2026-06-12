#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// Number of shadow cascades for multi-frustum culling.
	// Must match kShadowCascadeCount in FrameConstants.hpp.
	inline constexpr std::uint32_t kCullMultiFrustumCount = 3u;

	// -------------------------------------------------------------------------
	// Draw Contracts
	// -------------------------------------------------------------------------
	// Per-instance payload and push constants for mesh rendering.
	// Sync with shaders/include/RenderContracts.slangh.

	namespace DrawContracts
	{
		struct InstanceData
		{
			glm::mat4 model{1.0f};
			std::uint32_t materialIndex = 0xFFFFFFFFu;
			std::uint32_t skinPaletteOffset = 0;
			std::uint32_t skinJointCount = 0;
			std::uint32_t _pad0 = 0;
			glm::vec4 worldBoundingSphere{};
			gpu::DeviceAddress vertexBufferAddr = 0;
		};

		static_assert(sizeof(InstanceData) == 104, "InstanceData layout changed - update shaders/include/RenderContracts.slangh.");
		static_assert(offsetof(InstanceData, model) == 0);
		static_assert(offsetof(InstanceData, materialIndex) == 64);
		static_assert(offsetof(InstanceData, skinPaletteOffset) == 68);
		static_assert(offsetof(InstanceData, skinJointCount) == 72);
		static_assert(offsetof(InstanceData, _pad0) == 76);
		static_assert(offsetof(InstanceData, worldBoundingSphere) == 80);
		static_assert(offsetof(InstanceData, vertexBufferAddr) == 96);

		struct PushConstants
		{
			gpu::DeviceAddress frameAddr = 0;
			gpu::DeviceAddress instanceDataAddr = 0;
			gpu::DeviceAddress skinPaletteAddr = 0;
		};

		static_assert(sizeof(PushConstants) == 24, "PushConstants layout changed - update shaders/include/RenderContracts.slangh.");
		static_assert(offsetof(PushConstants, frameAddr) == 0);
		static_assert(offsetof(PushConstants, instanceDataAddr) == 8);
		static_assert(offsetof(PushConstants, skinPaletteAddr) == 16);
	} // namespace DrawContracts

	// -------------------------------------------------------------------------
	// Cull Contracts
	// -------------------------------------------------------------------------
	// Compute-based frustum culling input/output and push constants.
	// Sync with shaders/cull_draws.slang and cull_draws_multi.slang.

	namespace CullContracts
	{
		struct DrawInput
		{
			std::uint32_t indexCount = 0;
			std::uint32_t instanceCount = 1;
			std::uint32_t firstIndex = 0;
			std::int32_t vertexOffset = 0;
			std::uint32_t firstInstance = 0;
			std::uint32_t batchIndex = 0;
			std::uint32_t _pad[2]{};
		};

		static_assert(sizeof(DrawInput) == 32, "DrawInput layout changed - update shaders/include/RenderContracts.slangh.");
		static_assert(offsetof(DrawInput, indexCount) == 0);
		static_assert(offsetof(DrawInput, instanceCount) == 4);
		static_assert(offsetof(DrawInput, firstIndex) == 8);
		static_assert(offsetof(DrawInput, vertexOffset) == 12);
		static_assert(offsetof(DrawInput, firstInstance) == 16);
		static_assert(offsetof(DrawInput, batchIndex) == 20);
		static_assert(offsetof(DrawInput, _pad) == 24);

		struct Batch
		{
			std::uint32_t inputStart = 0;
			std::uint32_t drawCount = 0;
			std::uint32_t outputStart = 0;
			std::uint32_t _pad = 0;
		};

		static_assert(sizeof(Batch) == 16, "Batch layout changed - update shaders/include/RenderContracts.slangh.");
		static_assert(offsetof(Batch, inputStart) == 0);
		static_assert(offsetof(Batch, drawCount) == 4);
		static_assert(offsetof(Batch, outputStart) == 8);
		static_assert(offsetof(Batch, _pad) == 12);

		struct PushConstants
		{
			gpu::DeviceAddress frameAddr = 0;
			gpu::DeviceAddress instanceDataAddr = 0;
			gpu::DeviceAddress inputCmdAddr = 0;
			gpu::DeviceAddress outputCmdAddr = 0;
			gpu::DeviceAddress batchDescAddr = 0;
			gpu::DeviceAddress batchCountAddr = 0;
			std::uint32_t totalDrawCount = 0;
			std::uint32_t debugFlags = 0;
			std::uint32_t _pad0 = 0;
			std::uint32_t _pad1 = 0;
		};

		static_assert(sizeof(PushConstants) == 64, "PushConstants layout changed - update shaders/include/RenderContracts.slangh.");
		static_assert(offsetof(PushConstants, frameAddr) == 0);
		static_assert(offsetof(PushConstants, instanceDataAddr) == 8);
		static_assert(offsetof(PushConstants, inputCmdAddr) == 16);
		static_assert(offsetof(PushConstants, outputCmdAddr) == 24);
		static_assert(offsetof(PushConstants, batchDescAddr) == 32);
		static_assert(offsetof(PushConstants, batchCountAddr) == 40);
		static_assert(offsetof(PushConstants, totalDrawCount) == 48);
		static_assert(offsetof(PushConstants, debugFlags) == 52);
		static_assert(offsetof(PushConstants, _pad0) == 56);
		static_assert(offsetof(PushConstants, _pad1) == 60);

		struct MultiPushConstants
		{
			gpu::DeviceAddress frameAddrs[kCullMultiFrustumCount] = {};
			gpu::DeviceAddress instanceDataAddr = 0;
			gpu::DeviceAddress inputCmdAddr = 0;
			gpu::DeviceAddress outputCmdAddr = 0;
			gpu::DeviceAddress batchDescAddr = 0;
			std::uint32_t totalDrawCount = 0;
			std::uint32_t outputCascadeStride = 0;
			std::uint32_t debugFlags = 0;
			std::uint32_t _pad0 = 0;
		};

		static_assert(sizeof(MultiPushConstants) <= 128, "MultiPushConstants exceeds 128-byte push constant limit.");
		static_assert(offsetof(MultiPushConstants, frameAddrs) == 0);
		static_assert(offsetof(MultiPushConstants, instanceDataAddr) == 24);
		static_assert(offsetof(MultiPushConstants, inputCmdAddr) == 32);
		static_assert(offsetof(MultiPushConstants, outputCmdAddr) == 40);
		static_assert(offsetof(MultiPushConstants, batchDescAddr) == 48);
		static_assert(offsetof(MultiPushConstants, totalDrawCount) == 56);
		static_assert(offsetof(MultiPushConstants, outputCascadeStride) == 60);
		static_assert(offsetof(MultiPushConstants, debugFlags) == 64);
		static_assert(offsetof(MultiPushConstants, _pad0) == 68);

		inline constexpr std::uint32_t kDebugForceVisibleBit = 1u << 0;
	} // namespace CullContracts

	// -------------------------------------------------------------------------
	// Animation Contracts
	// -------------------------------------------------------------------------
	// GPU-driven animation skinning: clip sampling, pose flattening, skin palette.
	// Sync with shaders/include/AnimationContracts.slangh.

	namespace AnimationContracts
	{
		struct SkinCopyJob
		{
			gpu::DeviceAddress sampledPosesAddr = 0;
			std::uint32_t dstPaletteOffset = 0;
			std::uint32_t jointCount = 0;
			std::uint32_t skinIndex = 0;
			std::uint32_t nodeCount = 0;
			std::uint32_t nodePoseOffset = 0;
			std::uint32_t _pad1 = 0;
		};

		static_assert(sizeof(SkinCopyJob) == 32, "SkinCopyJob layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(SkinCopyJob, sampledPosesAddr) == 0);
		static_assert(offsetof(SkinCopyJob, dstPaletteOffset) == 8);
		static_assert(offsetof(SkinCopyJob, jointCount) == 12);
		static_assert(offsetof(SkinCopyJob, skinIndex) == 16);
		static_assert(offsetof(SkinCopyJob, nodeCount) == 20);
		static_assert(offsetof(SkinCopyJob, nodePoseOffset) == 24);
		static_assert(offsetof(SkinCopyJob, _pad1) == 28);

		struct AnimatorSampleJob
		{
			std::uint32_t animClipIndex = 0;
			float animTime = 0.0f;
			std::uint32_t nodePoseOffset = 0;
			std::uint32_t nodeCount = 0;
			gpu::DeviceAddress clipsAddr = 0;
			gpu::DeviceAddress channelsAddr = 0;
			gpu::DeviceAddress timesAddr = 0;
			gpu::DeviceAddress valuesAddr = 0;
			std::uint32_t clipCount = 0;
			std::uint32_t _pad0 = 0;
		};

		static_assert(sizeof(AnimatorSampleJob) == 56, "AnimatorSampleJob layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(AnimatorSampleJob, animClipIndex) == 0);
		static_assert(offsetof(AnimatorSampleJob, animTime) == 4);
		static_assert(offsetof(AnimatorSampleJob, nodePoseOffset) == 8);
		static_assert(offsetof(AnimatorSampleJob, nodeCount) == 12);
		static_assert(offsetof(AnimatorSampleJob, clipsAddr) == 16);
		static_assert(offsetof(AnimatorSampleJob, channelsAddr) == 24);
		static_assert(offsetof(AnimatorSampleJob, timesAddr) == 32);
		static_assert(offsetof(AnimatorSampleJob, valuesAddr) == 40);
		static_assert(offsetof(AnimatorSampleJob, clipCount) == 48);
		static_assert(offsetof(AnimatorSampleJob, _pad0) == 52);

		struct SampledNodePose
		{
			glm::vec4 translation{0.0f, 0.0f, 0.0f, 0.0f};
			glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};
			glm::vec4 scale{1.0f, 1.0f, 1.0f, 0.0f};
		};

		static_assert(sizeof(SampledNodePose) == 48, "SampledNodePose layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(SampledNodePose, translation) == 0);
		static_assert(offsetof(SampledNodePose, rotation) == 16);
		static_assert(offsetof(SampledNodePose, scale) == 32);

		struct SkinPalettePush
		{
			gpu::DeviceAddress jobsAddr = 0;
			gpu::DeviceAddress dstPaletteAddr = 0;
			gpu::DeviceAddress globalTransformsAddr = 0;
			gpu::DeviceAddress skinMetasAddr = 0;
			gpu::DeviceAddress skinJointsAddr = 0;
			gpu::DeviceAddress skinInverseBindsAddr = 0;
			std::uint32_t jobCount = 0;
			std::uint32_t _pad0 = 0;
			std::uint32_t _reserved0 = 0;
			std::uint32_t _reserved1 = 0;
		};

		static_assert(sizeof(SkinPalettePush) == 64, "SkinPalettePush layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(SkinPalettePush, jobsAddr) == 0);
		static_assert(offsetof(SkinPalettePush, dstPaletteAddr) == 8);
		static_assert(offsetof(SkinPalettePush, globalTransformsAddr) == 16);
		static_assert(offsetof(SkinPalettePush, skinMetasAddr) == 24);
		static_assert(offsetof(SkinPalettePush, skinJointsAddr) == 32);
		static_assert(offsetof(SkinPalettePush, skinInverseBindsAddr) == 40);
		static_assert(offsetof(SkinPalettePush, jobCount) == 48);
		static_assert(offsetof(SkinPalettePush, _reserved0) == 56);
		static_assert(offsetof(SkinPalettePush, _reserved1) == 60);

		struct NodeFlattenPush
		{
			gpu::DeviceAddress sampledPosesAddr = 0;
			gpu::DeviceAddress nodeParentsAddr = 0;
			gpu::DeviceAddress globalTransformsAddr = 0;
			gpu::DeviceAddress depthSortedNodesAddr = 0;
			gpu::DeviceAddress animatorJobsAddr = 0;
			std::uint32_t depthOffset = 0;
			std::uint32_t nodeCount = 0;
			std::uint32_t batchStartJob = 0;
			std::uint32_t batchJobCount = 0;
		};

		static_assert(sizeof(NodeFlattenPush) == 56, "NodeFlattenPush layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(NodeFlattenPush, sampledPosesAddr) == 0);
		static_assert(offsetof(NodeFlattenPush, nodeParentsAddr) == 8);
		static_assert(offsetof(NodeFlattenPush, globalTransformsAddr) == 16);
		static_assert(offsetof(NodeFlattenPush, depthSortedNodesAddr) == 24);
		static_assert(offsetof(NodeFlattenPush, animatorJobsAddr) == 32);
		static_assert(offsetof(NodeFlattenPush, depthOffset) == 40);
		static_assert(offsetof(NodeFlattenPush, nodeCount) == 44);
		static_assert(offsetof(NodeFlattenPush, batchStartJob) == 48);
		static_assert(offsetof(NodeFlattenPush, batchJobCount) == 52);

		struct AnimationSamplePush
		{
			gpu::DeviceAddress animDbClipsAddr = 0;
			gpu::DeviceAddress animDbChannelsAddr = 0;
			gpu::DeviceAddress animDbTimesAddr = 0;
			gpu::DeviceAddress animDbValuesAddr = 0;
			gpu::DeviceAddress bindTranslationsAddr = 0;
			gpu::DeviceAddress bindRotationsAddr = 0;
			gpu::DeviceAddress bindScalesAddr = 0;
			gpu::DeviceAddress animatorJobsAddr = 0;
			gpu::DeviceAddress sampledPosesAddr = 0;
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

		struct PoseInitPush
		{
			gpu::DeviceAddress bindTranslationsAddr = 0;
			gpu::DeviceAddress bindRotationsAddr = 0;
			gpu::DeviceAddress bindScalesAddr = 0;
			gpu::DeviceAddress animatorJobsAddr = 0;
			gpu::DeviceAddress sampledPosesAddr = 0;
			std::uint32_t jobCount = 0;
			std::uint32_t nodeCountPerJob = 0;
		};

		static_assert(sizeof(PoseInitPush) == 48, "PoseInitPush layout changed - update shaders/include/AnimationContracts.slangh.");
		static_assert(offsetof(PoseInitPush, bindTranslationsAddr) == 0);
		static_assert(offsetof(PoseInitPush, bindRotationsAddr) == 8);
		static_assert(offsetof(PoseInitPush, bindScalesAddr) == 16);
		static_assert(offsetof(PoseInitPush, animatorJobsAddr) == 24);
		static_assert(offsetof(PoseInitPush, sampledPosesAddr) == 32);
		static_assert(offsetof(PoseInitPush, jobCount) == 40);
		static_assert(offsetof(PoseInitPush, nodeCountPerJob) == 44);

		// Extended sample job with secondary-clip blending support.
		// primaryClipIndex/primaryTime sample the base animation.
		// When secondaryClipIndex is non-zero and blendWeight < 1.0, the GPU
		// samples secondaryClipIndex/secondaryTime and lerps the resulting
		// translations by (1 - blendWeight), leaving rotations slerped.
		struct AnimatorBlendJob
		{
			std::uint32_t primaryClipIndex = 0;
			float primaryTime = 0.0f;
			std::uint32_t secondaryClipIndex = 0;
			float secondaryTime = 0.0f;
			float blendWeight = 1.0f; // 1.0=fully primary, 0.0=fully secondary
			std::uint32_t nodePoseOffset = 0;
			std::uint32_t nodeCount = 0;
			gpu::DeviceAddress clipsAddr = 0;
			gpu::DeviceAddress channelsAddr = 0;
			gpu::DeviceAddress timesAddr = 0;
			gpu::DeviceAddress valuesAddr = 0;
			std::uint32_t clipCount = 0;
			std::uint32_t _pad0 = 0;
		};

		static_assert(sizeof(AnimatorBlendJob) == 72, "AnimatorBlendJob layout changed - update shaders/include/AnimationContracts.slangh.");

		// Push constant for the anim blend compute pass.
		// The blend pass reads AnimatorBlendJob entries from a GPU buffer, samples
		// two clip poses and lerps them, writing flat node poses to sampledPosesAddr.
		struct AnimationBlendPush
		{
			gpu::DeviceAddress animDbClipsAddr = 0;
			gpu::DeviceAddress animDbChannelsAddr = 0;
			gpu::DeviceAddress animDbTimesAddr = 0;
			gpu::DeviceAddress animDbValuesAddr = 0;
			gpu::DeviceAddress bindTranslationsAddr = 0;
			gpu::DeviceAddress bindRotationsAddr = 0;
			gpu::DeviceAddress bindScalesAddr = 0;
			gpu::DeviceAddress blendJobsAddr = 0;
			gpu::DeviceAddress sampledPosesAddr = 0;
			std::uint32_t jobCount = 0;
			std::uint32_t _pad0 = 0;
		};

		static_assert(sizeof(AnimationBlendPush) == 80, "AnimationBlendPush layout changed - update shaders/include/AnimationContracts.slangh.");

		// Per-entity IK solve job. Each thread handles one entity's two-bone leg IK.
		// Bone indices are looked up once at spawn and stored in IkTargetsComponent.
		// The pass reads global transforms (which include animation + blend), applies
		// foot-ground corrections computed by the CPU raycast pass, and writes
		// corrected global transforms back so the skinned mesh renders the planted feet.
		struct IkSolveJob
		{
			std::uint32_t hipsNodeIdx = UINT32_MAX;
			std::uint32_t leftKneeNodeIdx = UINT32_MAX;
			std::uint32_t leftFootNodeIdx = UINT32_MAX;
			std::uint32_t rightKneeNodeIdx = UINT32_MAX;
			std::uint32_t rightFootNodeIdx = UINT32_MAX;
			float leftUpperLegLen = 0.0f;
			float leftLowerLegLen = 0.0f;
			float rightUpperLegLen = 0.0f;
			float rightLowerLegLen = 0.0f;
			float leftKneeBendSign = 1.0f;
			float rightKneeBendSign = 1.0f;
			gpu::DeviceAddress globalTransformsAddr = 0;
			gpu::DeviceAddress ikResultsAddr = 0; // CPU-written per-entity foot ground data
			std::uint32_t entityId = 0;
			std::uint32_t _pad0 = 0;
		};

		static_assert(sizeof(IkSolveJob) == 72, "IkSolveJob layout changed - update shaders/include/AnimationContracts.slangh.");

		// Push constant for the IK solve compute pass.
		struct IkSolvePush
		{
			gpu::DeviceAddress globalTransformsAddr = 0;
			gpu::DeviceAddress ikJobsAddr = 0;
			gpu::DeviceAddress ikGroundResultsAddr = 0; // GPU buffer of IkGroundResult entries
			gpu::DeviceAddress nodeParentsAddr = 0;
			gpu::DeviceAddress depthSortedNodesAddr = 0;
			std::uint32_t jobCount = 0;
			std::uint32_t nodeCount = 0;
		};

		static_assert(sizeof(IkSolvePush) == 48, "IkSolvePush layout changed - update shaders/include/AnimationContracts.slangh.");

		// Per-entity IK ground result written by CPU and read by GPU IK solver.
		// Two entries per entity: [0] = left foot, [1] = right foot.
		struct IkGroundResult
		{
			std::uint32_t entityId = 0;
			std::uint32_t footIndex = 0; // 0=left, 1=right
			float groundY = 0.0f;
			float footOffsetY = 0.0f; // vertical correction to apply (0 = in air)
			std::uint32_t _pad0 = 0;
			std::uint32_t _pad1 = 0;
		};

		static_assert(sizeof(IkGroundResult) == 24, "IkGroundResult layout changed - update shaders/include/AnimationContracts.slangh.");
	} // namespace AnimationContracts
} // namespace aether
