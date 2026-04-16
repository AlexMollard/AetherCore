#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	// Per-instance payload consumed by the standard mesh shader path.
	// Packed into a persistently-mapped storage buffer and indexed via
	// firstInstance / SV_InstanceID so indirect draws can drive the same shader.
	//
	//   offset  0 : mat4    model                (64 bytes)
	//   offset 64 : uint32  materialIndex        ( 4 bytes)
	//   offset 68 : uint32  _pad0                ( 4 bytes)
	//   offset 72 : uint64  skinBufferAddr       ( 8 bytes)
	//   offset 80 : vec4    worldBoundingSphere  (16 bytes) xyz=center, w=radius; w<=0 → skip culling
	struct DrawInstanceData
	{
		glm::mat4 model{ 1.0f };
		std::uint32_t materialIndex = 0xFFFFFFFFu;
		std::uint32_t _pad0 = 0;
		VkDeviceAddress skinBufferAddr = 0;
		glm::vec4 worldBoundingSphere{}; // xyz=world center, w=radius; w<=0 → skip frustum culling
	};

	static_assert(sizeof(DrawInstanceData) == 96, "DrawInstanceData layout changed — update gltf_mesh.slang and cull_draws.slang.");

	// Push constants for standard mesh draws (vertex + fragment shaders).
	//
	//   offset  0 : uint64 frameAddr         (BDA of FrameConstants)
	//   offset  8 : uint64 instanceDataAddr  (BDA of DrawInstanceData[])
	//
	// Total: 16 bytes.
	struct DrawPushConstants
	{
		VkDeviceAddress frameAddr = 0;
		VkDeviceAddress instanceDataAddr = 0;
	};

	static_assert(sizeof(DrawPushConstants) == 16, "DrawPushConstants layout changed — update gltf_mesh.slang.");

	// Input to the cull compute shader — one entry per submitted draw.
	// Contains the prototype VkDrawIndexedIndirectCommand fields plus the batch index.
	//
	//   offset  0 : uint32  indexCount
	//   offset  4 : uint32  instanceCount  (always 1)
	//   offset  8 : uint32  firstIndex
	//   offset 12 : int32   vertexOffset
	//   offset 16 : uint32  firstInstance  (index into DrawInstanceData[])
	//   offset 20 : uint32  batchIndex     (which output batch this draw belongs to)
	//   offset 24 : uint32  _pad[2]
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

	static_assert(sizeof(CullDrawInput) == 32, "CullDrawInput layout changed — update cull_draws.slang.");

	// Per-batch descriptor written by CPU, read by the cull compute shader.
	// Describes the input range and output range in the indirect command buffers.
	//
	//   offset  0 : uint32  inputStart   (first draw in CullDrawInput[])
	//   offset  4 : uint32  drawCount    (number of draws = max output capacity for this batch)
	//   offset  8 : uint32  outputStart  (first slot in output VkDrawIndexedIndirectCommand[])
	//   offset 12 : uint32  _pad
	struct CullBatch
	{
		std::uint32_t inputStart = 0;
		std::uint32_t drawCount = 0;
		std::uint32_t outputStart = 0;
		std::uint32_t _pad = 0;
	};

	static_assert(sizeof(CullBatch) == 16, "CullBatch layout changed — update cull_draws.slang.");

	// Push constants for the cull compute shader.
	//
	//   offset  0 : uint64  frameAddr         BDA → FrameConstantsData (for viewProj frustum planes)
	//   offset  8 : uint64  instanceDataAddr  BDA → DrawInstanceData[]  (for bounding spheres)
	//   offset 16 : uint64  inputCmdAddr      BDA → CullDrawInput[]     (prototype commands)
	//   offset 24 : uint64  outputCmdAddr     BDA → uint32[]            (VkDrawIndexedIndirectCommand array)
	//   offset 32 : uint64  batchDescAddr     BDA → CullBatch[]
	//   offset 40 : uint64  batchCountAddr    (reserved)
	//   offset 48 : uint32  totalDrawCount
	//   offset 52 : uint32  debugFlags        bit0=force all draws visible
	//   offset 56 : uint32  _pad0
	//   offset 60 : uint32  _pad1
	//
	// Total: 64 bytes.
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

	static_assert(sizeof(CullPushConstants) == 64, "CullPushConstants layout changed — update cull_draws.slang.");
} // namespace aether
