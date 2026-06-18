#pragma once

#include <cstdint>
#include <type_traits>

#include "gpu/GpuFormat.hpp"

// -------------------------------------------------------------------------
// GpuEnums - Vulkan-free engine-facing RHI enums
// -------------------------------------------------------------------------
// Engine-facing RHI enums for pipeline stages, access flags, shader stages,
// descriptor types, image layouts, and related types. Each enum lives in
// namespace aether::gpu and is mapped to its Vk* equivalent in
// vulkan/GpuEnumConversions.cpp via ToVk() / FromVk().

namespace aether::gpu
{
	// ---------------------------------------------------------------------
	// GENERIC BITWISE OPERATORS
	// ---------------------------------------------------------------------
	// Automatically handles 32-bit and 64-bit enums safely without manual casts.
	// (Requires C++17 for std::is_enum_v.)

	template<typename Enum>
	constexpr Enum operator|(Enum lhs, Enum rhs) noexcept
	    requires(std::is_enum_v<Enum>)
	{
		using Underlying = std::underlying_type_t<Enum>;
		return static_cast<Enum>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));
	}

	template<typename Enum>
	constexpr Enum& operator|=(Enum& lhs, Enum rhs) noexcept
	    requires(std::is_enum_v<Enum>)
	{
		lhs = lhs | rhs;
		return lhs;
	}

	template<typename Enum>
	constexpr Enum operator&(Enum lhs, Enum rhs) noexcept
	    requires(std::is_enum_v<Enum>)
	{
		using Underlying = std::underlying_type_t<Enum>;
		return static_cast<Enum>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));
	}

	template<typename Enum>
	constexpr Enum& operator&=(Enum& lhs, Enum rhs) noexcept
	    requires(std::is_enum_v<Enum>)
	{
		lhs = lhs & rhs;
		return lhs;
	}

	template<typename Enum>
	constexpr Enum operator~(Enum rhs) noexcept
	    requires(std::is_enum_v<Enum>)
	{
		using Underlying = std::underlying_type_t<Enum>;
		return static_cast<Enum>(~static_cast<Underlying>(rhs));
	}

	// ---------------------------------------------------------------------
	// ENUMS & STRUCTS
	// ---------------------------------------------------------------------

	enum class IndexType : std::uint32_t
	{
		U16 = 0,
		U32 = 1,
	};

	// Shader stage(s) for push-constant ranges. Mapped to VkShaderStageFlags
	// in the backend via a manual bit-test; bit positions do NOT match
	// VkShaderStageFlagBits.
	enum class ShaderStage : std::uint32_t
	{
		None = 0,
		Vertex = 1u << 0,
		Fragment = 1u << 1,
		Compute = 1u << 2,
		// Convenience aggregates used by the engine today.
		AllGraphics = Vertex | Fragment,
		All = Vertex | Fragment | Compute,
	};

	// Pipeline bind point for the backend's bind-pipeline and push-descriptor commands.
	// Mirrors VkPipelineBindPoint; the backend maps to VK_PIPELINE_BIND_POINT_*.
	// The graphics variant is the default for descriptor set / push descriptor
	// calls; compute pipelines must use Compute to satisfy
	// Validation rule for the cached-bind-point push-descriptor path.
	enum class PipelineBindPoint : std::uint32_t
	{
		Graphics = 0,
		Compute = 1,
	};

	// Pipeline stage bits for memory barriers.
	// Mapped to VkPipelineStageFlags2 in the backend. Enumerates the
	// subset of VkPipelineStageFlagBits2 the engine actually uses.
	enum class PipelineStage : std::uint64_t
	{
		None = 0,
		DrawIndirect = 1ull << 1,
		VertexInput = 1ull << 2,
		VertexShader = 1ull << 3,
		FragmentShader = 1ull << 7,
		ComputeShader = 1ull << 11,
		Host = 1ull << 14,
		AllCommands = 1ull << 16,
		Transfer = 1ull << 12,
	};

	// Memory access bits for barriers.
	// Mapped to VkAccessFlags2 in the backend. Enumerates the
	// subset of VkAccessFlagBits2 the engine actually uses.
	enum class AccessFlags : std::uint64_t
	{
		None = 0,
		IndirectCommandRead = 1ull << 0,
		IndexRead = 1ull << 1,
		VertexAttributeRead = 1ull << 2,
		ShaderRead = 1ull << 5,
		ShaderWrite = 1ull << 6,
		TransferRead = 1ull << 11,
		TransferWrite = 1ull << 12,
		HostWrite = 1ull << 14,
		ShaderStorageRead = 1ull << 33,
		ShaderStorageWrite = 1ull << 34,
	};

	// Descriptor type bits used in push-descriptor writes.
	// Mirrors VkDescriptorType. The engine uses:
	//   - StorageBuffer        (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
	enum class DescriptorType : std::uint32_t
	{
		Sampler = 0,
		CombinedImageSampler = 1,
		SampledImage = 2,
		StorageImage = 3,
		UniformTexelBuffer = 4,
		StorageTexelBuffer = 5,
		StorageBuffer = 6,
		UniformBuffer = 7,
		UniformBufferDynamic = 8,
		StorageBufferDynamic = 9,
		InputAttachment = 10,
	};

	// Bitflags for descriptor-set layout creation (mirrors
	// VkDescriptorSetLayoutCreateFlagBits, the only flag the engine uses
	// is the push-descriptor bit).
	enum class DescriptorSetLayoutFlags : std::uint32_t
	{
		None = 0,
		PushDescriptor = 1u << 0,
	};

	// Depth / stencil compare operations. Mapped to VkCompareOp.
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

	// Load operation for color / depth attachments.
	// Mirrors VkAttachmentLoadOp.
	enum class LoadOp : std::uint32_t
	{
		Load = 0,
		Clear = 1,
		DontCare = 2,
	};

	// Store operation for color / depth attachments.
	// Mirrors VkAttachmentStoreOp.
	enum class StoreOp : std::uint32_t
	{
		Store = 0,
		DontCare = 1,
	};

	// Buffer usage flags. Mirrors VkBufferUsageFlagBits. Bitwise-OR-able.
	enum class BufferUsage : std::uint32_t
	{
		None = 0,
		TransferSrc = 1u << 0,
		TransferDst = 1u << 1,
		UniformTexel = 1u << 2,
		StorageTexel = 1u << 3,
		Uniform = 1u << 4,
		Storage = 1u << 5,
		Index = 1u << 6,
		Vertex = 1u << 7,
		Indirect = 1u << 8,
		ShaderDeviceAddress = 1u << 17,
		// VK_BUFFER_USAGE_2_DESCRIPTOR_HEAP_BIT_EXT (0x10000000). Heap backing
		// buffer for VK_EXT_descriptor_heap — host-visible, persistently mapped,
		// read by the GPU via BDA when bound as a resource/sampler heap.
		DescriptorHeap = 1u << 28,
	};

	// Image usage flags. Mirrors VkImageUsageFlags. Bitwise-OR-able.
	enum class ImageUsage : std::uint32_t
	{
		None = 0,
		TransferSrc = 1u << 0,
		TransferDst = 1u << 1,
		Sampled = 1u << 2,
		Storage = 1u << 3,
		ColorAttachment = 1u << 4,
		DepthStencilAttachment = 1u << 5,
		// Required for vkCopyMemoryToImage / vkTransitionImageLayout
		// (Vulkan 1.4 host image copy). Opt-in at the call site that intends
		// to do a host-side upload via those entry points.
		HostTransfer = 1u << 6,
	};

	// Image aspect flags. Mirrors VkImageAspectFlags. Bitwise-OR-able.
	enum class ImageAspect : std::uint32_t
	{
		None = 0,
		Color = 1u << 0,
		Depth = 1u << 1,
		Stencil = 1u << 2,
	};

	// Image layout. Mirrors VkImageLayout. Only the layouts the engine's
	// barrier solver emits are enumerated; new layouts force a switch-case
	// compile error in the backend conversion.
	enum class ImageLayout : std::uint32_t
	{
		Undefined = 0,
		General = 1,
		ColorAttachment = 2,
		DepthAttachment = 3,
		ShaderReadOnly = 4,
		TransferSrc = 5,
		TransferDst = 6,
	};

	// Component swizzle applied at view-creation time. Mirrors
	// VkComponentSwizzle. Identity preserves the format; Zero returns 0
	// in that channel; One returns 1; R/G/B/A select a specific source
	// channel. Used to expand R8_UNORM to RGBA8 in bindless descriptors
	// (e.g. font SDF atlases). The ToVk() conversion lives in
	// vulkan/GpuEnumConversions.hpp.
	enum class ComponentSwizzle : std::uint32_t
	{
		Identity = 0,
		Zero = 1,
		One = 2,
		R = 3,
		G = 4,
		B = 5,
		A = 6,
	};

	// Mapped buffer memory usage. Selects the backing memory pool and the
	// VMA host-access flag pattern. Mirrors a subset of VmaMemoryUsage:
	//   - GpuToCpu: device-local memory with host access (readback)
	//   - CpuToGpu: host-visible memory optimized for CPU writes (upload)
	//   - Auto:     let VMA pick (default for the existing CreateMappedBuffer
	//               usage: ASSET_UPLOAD with HOST_ACCESS_SEQUENTIAL_WRITE)
	enum class MappedMemoryUsage : std::uint32_t
	{
		Auto = 0,
		CpuToGpu = 1,
		GpuToCpu = 2,
	};

	// Texture filter. Mirrors VkFilter. Only the values BindlessManager's
	// sampler cache emits are enumerated; new filters force a switch-case
	// compile error in the backend conversion.
	enum class Filter : std::uint32_t
	{
		Nearest = 0,
		Linear = 1,
	};

	// Sampler mipmap mode. Mirrors VkSamplerMipmapMode.
	enum class SamplerMipmapMode : std::uint32_t
	{
		Nearest = 0,
		Linear = 1,
	};

	// Sampler address mode (UVW wrap mode). Mirrors VkSamplerAddressMode.
	// Only the values BindlessManager's sampler cache emits are enumerated.
	enum class SamplerAddressMode : std::uint32_t
	{
		Repeat = 0,
		MirroredRepeat = 1,
		ClampToEdge = 2,
		ClampToBorder = 3,
	};

	// Clear value for a color or depth/stencil attachment. Mirrors
	// VkClearValue's union layout. Construct with gpu::ClearColor(r,g,b,a) or
	// gpu::ClearDepth(depth, stencil) factory helpers.
	struct ClearValue
	{
		float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float depth = 0.0f;
		std::uint32_t stencil = 0u;
	};

	[[nodiscard]] inline ClearValue ClearColor(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept
	{
		ClearValue v{};
		v.color[0] = r;
		v.color[1] = g;
		v.color[2] = b;
		v.color[3] = a;
		return v;
	}

	[[nodiscard]] inline ClearValue ClearDepth(float depth = 1.0f, std::uint32_t stencil = 0u) noexcept
	{
		ClearValue v{};
		v.depth = depth;
		v.stencil = stencil;
		return v;
	}

	// 2D extent (width, height). Mirrors VkExtent2D. Constructible from any
	// type exposing .width/.height (including VkExtent2D and the legacy
	// aether::GpuExtent2D typedef). The templated converting constructor
	// is intentionally non-explicit so engine code can pass
	// GpuDevice::GetSwapchainExtent() (which returns GpuExtent2D)
	// directly to functions that take gpu::Extent2D.
	struct Extent2D
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		Extent2D() = default;

		Extent2D(std::uint32_t w, std::uint32_t h) noexcept
		      : width(w), height(h)
		{
		}

		template<typename Other>
		Extent2D(const Other& ext) noexcept
		      : width(ext.width), height(ext.height)
		{
		}
	};

	// Primitive topology. Mirrors VkPrimitiveTopology. Only the topologies the
	// engine's pipeline factory emits are enumerated.
	enum class PrimitiveTopology : std::uint32_t
	{
		PointList = 0,
		LineList = 1,
		TriangleList = 2,
	};

	// Polygon fill mode. Mirrors VkPolygonMode.
	enum class PolygonMode : std::uint32_t
	{
		Fill = 0,
		Line = 1,
	};

	// Per-vertex input binding description. Mirrors
	// VkVertexInputBindingDescription. The engine does not expose vertex-input
	// rate enums in the gpu/ layer; just use 0 for per-vertex, 1 for per-instance
	// (matching the VK_VERTEX_INPUT_RATE_* values).
	struct VertexInputBinding
	{
		std::uint32_t binding = 0;
		std::uint32_t stride = 0;
		std::uint32_t inputRate = 0; // 0=VERTEX, 1=INSTANCE
	};

	// Per-vertex attribute description. Mirrors VkVertexInputAttributeDescription.
	struct VertexInputAttribute
	{
		std::uint32_t location = 0;
		std::uint32_t binding = 0;
		Format format = Format::Undefined;
		std::uint32_t offset = 0;
	};
} // namespace aether::gpu

namespace aether
{
	// Engine-side texture filter.
	// so engine code can include GpuEnums.hpp without dragging in the
	// Vulkan backend.
	enum class TextureFilter
	{
		Linear,
		Nearest,
	};
} // namespace aether
