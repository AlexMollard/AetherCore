#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────
// GpuEnums - Vulkan-free engine-facing RHI enums
// ─────────────────────────────────────────────────────────────────────────
// Phase 1 introduced gpu::Format. Phase 3 needs a few more: the index-buffer
// element type (used by CommandList::BindIndexBuffer) and shader-stage flags
// (used by CommandList::PushConstantsRaw). All live in namespace aether::gpu
// and are mapped to Vk* in vulkan/GpuEnumConversions.cpp alongside ToVk(Format).
//
// These types are intentionally additive: nothing in the engine depends on
// them yet, so adding them is non-breaking. Callers are migrated one at a
// time (Phase 3 = passes, Phase 5 = RenderGraph barriers).

namespace aether::gpu
{
	// Element type of a bound index buffer.
	enum class IndexType : std::uint32_t
	{
		U16 = 0,
		U32 = 1,
	};

	// Shader stage(s) for push-constant ranges. Mirrors VkShaderStageFlagBits
	// subset the engine actually uses; the backend maps the bitwise union
	// straight to VkShaderStageFlags.
	enum class ShaderStage : std::uint32_t
	{
		None = 0,
		Vertex = 1u << 0,
		Fragment = 1u << 1,
		Compute = 1u << 2,
		// Convenience aggregates used by the engine today.
		VertexFragment = Vertex | Fragment,
		AllGraphics = Vertex | Fragment,
		All = Vertex | Fragment | Compute,
	};

	inline ShaderStage operator|(ShaderStage a, ShaderStage b) noexcept
	{
		return static_cast<ShaderStage>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
	}

	inline ShaderStage& operator|=(ShaderStage& a, ShaderStage b) noexcept
	{
		a = a | b;
		return a;
	}

	inline ShaderStage operator&(ShaderStage a, ShaderStage b) noexcept
	{
		return static_cast<ShaderStage>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
	}

	// Pipeline bind point for vkCmdBindPipeline / vkCmdPushDescriptorSet*.
	// Mirrors VkPipelineBindPoint; the backend maps to VK_PIPELINE_BIND_POINT_*.
	// The graphics variant is the default for descriptor set / push descriptor
	// calls; compute pipelines must use Compute to satisfy
	// VUID-vkCmdPushDescriptorSet-pipelineBindPoint-00363.
	enum class PipelineBindPoint : std::uint32_t
	{
		Graphics = 0,
		Compute = 1,
	};

	// Pipeline stage bits for memory barriers.
	// Mirrors VkPipelineStageFlagBits2. The engine uses a small subset:
	//   - Host            (mapped from VK_PIPELINE_STAGE_2_HOST_BIT)
	//   - ComputeShader   (mapped from VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	//   - FragmentShader  (mapped from VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
	//   - AllCommands     (mapped from VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	//                      used for "anything before" pseudo-stages)
	// The backend maps these straight to VkPipelineStageFlags2.
	enum class PipelineStage : std::uint64_t
	{
		None = 0,
		Host = 1ull << 7,
		VertexShader = 1ull << 9,
		FragmentShader = 1ull << 11,
		ComputeShader = 1ull << 19,
		AllCommands = 1ull << 27,
	};

	inline PipelineStage operator|(PipelineStage a, PipelineStage b) noexcept
	{
		return static_cast<PipelineStage>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
	}

	inline PipelineStage& operator|=(PipelineStage& a, PipelineStage b) noexcept
	{
		a = a | b;
		return a;
	}

	inline PipelineStage operator&(PipelineStage a, PipelineStage b) noexcept
	{
		return static_cast<PipelineStage>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
	}

	// Memory access bits for barriers.
	// Mirrors VkAccessFlagBits2. The engine uses:
	//   - HostWrite               (VK_ACCESS_2_HOST_WRITE_BIT)
	//   - ShaderStorageRead       (VK_ACCESS_2_SHADER_STORAGE_READ_BIT)
	//   - ShaderStorageWrite      (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
	enum class AccessFlags : std::uint64_t
	{
		None = 0,
		HostWrite = 1ull << 14,
		ShaderStorageRead = 1ull << 22,
		ShaderStorageWrite = 1ull << 23,
	};

	inline AccessFlags operator|(AccessFlags a, AccessFlags b) noexcept
	{
		return static_cast<AccessFlags>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
	}

	inline AccessFlags& operator|=(AccessFlags& a, AccessFlags b) noexcept
	{
		a = a | b;
		return a;
	}

	inline AccessFlags operator&(AccessFlags a, AccessFlags b) noexcept
	{
		return static_cast<AccessFlags>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
	}

	// Descriptor type bits used in push-descriptor writes.
	// Mirrors VkDescriptorType. The engine uses:
	//   - StorageBuffer        (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
	enum class DescriptorType : std::uint32_t
	{
		StorageBuffer = 6,
	};

	// Bitflags for descriptor-set layout creation (mirrors
	// VkDescriptorSetLayoutCreateFlagBits, the only flag the engine uses
	// is the push-descriptor bit).
	enum class DescriptorSetLayoutFlags : std::uint32_t
	{
		None = 0,
		PushDescriptor = 1u << 0,
	};

	// Depth / stencil compare operations.
	// Mirrors VkCompareOp. The engine only ever uses Less / LessOrEqual -
	// add more if a future pass needs them.
	enum class CompareOp : std::uint32_t
	{
		Never = 0,
		Less = 1,
		Equal = 2,
		LessOrEqual = 3,
		Greater = 4,
		NotEqual = 5,
		GreaterOrEqual = 6,
		Always = 7,
	};
} // namespace aether::gpu
