#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
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

	static_assert(sizeof(SkinCopyJob) == 48, "SkinCopyJob layout changed - update skin_palette_build.slang.");
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

	static_assert(sizeof(AnimatorSampleJob) == 16, "AnimatorSampleJob layout changed - update animation_sample.slang.");
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

	static_assert(sizeof(SampledNodePose) == 48, "SampledNodePose layout changed - update animation_sample.slang.");
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

	static_assert(sizeof(SkinPalettePush) == 72, "SkinPalettePush layout changed - update skin_palette_build.slang.");
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

	static_assert(sizeof(AnimationSamplePush) == 80, "AnimationSamplePush layout changed - update animation_sample.slang.");
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
