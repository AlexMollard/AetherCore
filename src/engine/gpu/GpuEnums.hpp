#pragma once

#include <cstdint>
#include <type_traits>

#include "gpu/GpuFormat.hpp"

namespace aether::gpu
{
	// How finished frames reach the screen. This is a three-way choice, not a vsync bool:
	// Fifo and Mailbox both avoid tearing but queue very differently, and that queueing is
	// what the player feels as input lag.
	enum class PresentMode
	{
		Fifo,      // block until the next vsync; no tearing, but presents queue up behind each other
		Mailbox,   // replace the queued image instead of queueing behind it; no tearing, less lag, more GPU
		Immediate, // present on the spot; lowest lag and tears
	};

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

	enum class IndexType : std::uint32_t
	{
		U16 = 0,
		U32 = 1,
	};

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

	enum class BlendMode : std::uint32_t
	{
		Alpha = 0,
		Additive,
		Multiply,
		Opaque,
		// Source colour is already multiplied by its own coverage, so it is added
		// whole and the destination is attenuated by what the source covers. This is
		// how a participating medium composites: colour scattered toward the eye plus
		// whatever light survived the medium, in one blend.
		Premultiplied,
	};

	enum class LoadOp : std::uint32_t
	{
		Load = 0,
		Clear = 1,
		DontCare = 2,
	};

	enum class StoreOp : std::uint32_t
	{
		Store = 0,
		DontCare = 1,
	};

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
		DescriptorHeap = 1u << 28,
	};

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
		HostTransfer = 1u << 6,
	};

	enum class ImageAspect : std::uint32_t
	{
		None = 0,
		Color = 1u << 0,
		Depth = 1u << 1,
		Stencil = 1u << 2,
	};

	// Image layout. Mirrors VkImageLayout. Only the layouts the engine's
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

	enum class MappedMemoryUsage : std::uint32_t
	{
		Auto = 0,
		CpuToGpu = 1,
		GpuToCpu = 2,
	};

	enum class Filter : std::uint32_t
	{
		Nearest = 0,
		Linear = 1,
	};

	enum class SamplerMipmapMode : std::uint32_t
	{
		Nearest = 0,
		Linear = 1,
	};

	enum class SamplerAddressMode : std::uint32_t
	{
		Repeat = 0,
		MirroredRepeat = 1,
		ClampToEdge = 2,
		ClampToBorder = 3,
	};

	// VkClearValue's union layout. Construct with gpu::ClearColor(r,g,b,a) or
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

	enum class PrimitiveTopology : std::uint32_t
	{
		PointList = 0,
		LineList = 1,
		TriangleList = 2,
	};

	enum class PolygonMode : std::uint32_t
	{
		Fill = 0,
		Line = 1,
	};

	enum class CullMode : std::uint32_t
	{
		None = 0,
		Front = 1,
		Back = 2,
		FrontAndBack = 3,
	};

	struct VertexInputBinding
	{
		std::uint32_t binding = 0;
		std::uint32_t stride = 0;
		std::uint32_t inputRate = 0;
	};

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
	enum class TextureFilter
	{
		Linear,
		Nearest,
	};
}
