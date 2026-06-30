#pragma once

#include <cstdint>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/FrameTarget.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/FrameBlackboard.hpp"

namespace aether
{
	// Which hardware queue a pass executes on.
	enum class QueueClass : uint8_t
	{
		Graphics,     // main graphics queue (all rendering, inline compute)
		AsyncCompute, // dedicated async compute queue (culling, lighting)
	};

	// Opaque handle to a render-graph-managed image resource.
	// Acquired from RenderGraph::GetSwapchainColor/Depth or CreateTransient*.
	struct RGImage
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	// Opaque handle to a render-graph-managed buffer resource.
	struct RGBuffer
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	struct PreparedDrawList
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	inline constexpr std::string_view kFrameProductMainView = "MainView";
	inline constexpr std::string_view kFrameProductSceneDepth = "SceneDepth";
	inline constexpr std::string_view kFrameProductHdrColor = "HdrColor";
	inline constexpr std::string_view kFrameProductGtao = "GTAO";
	inline constexpr std::string_view kFrameProductDirectionalShadows = "DirectionalShadows";
	inline constexpr std::string_view kFrameProductLocalShadows = "LocalShadows";
	inline constexpr std::string_view kFrameProductLightBuffers = "LightBuffers";

	struct MainViewProduct
	{
		gpu::Extent2D extent{};
		std::uint64_t frameIndex = 0;
		std::uint32_t frameSlot = 0;
		std::uint64_t frameConstantsAddr = 0;
	};

	struct SceneDepthProduct
	{
		RGImage image{};
		gpu::Extent2D extent{};
		gpu::Format format = gpu::Format::Undefined;
		std::uint32_t bindlessSlot = UINT32_MAX;
	};

	struct HdrColorProduct
	{
		RGImage image{};
		gpu::Extent2D extent{};
		gpu::Format format = gpu::Format::Undefined;
		std::uint32_t bindlessSlot = UINT32_MAX;
	};

	struct GtaoProduct
	{
		RGImage image{};
		gpu::Extent2D extent{};
		gpu::Format format = gpu::Format::R8Unorm;
		std::uint32_t bindlessSlot = UINT32_MAX;
	};

	struct DirectionalShadowProduct
	{
		std::vector<RGImage> depthImages;
		std::vector<std::uint32_t> bindlessSlots;
	};

	struct LocalShadowProduct
	{
		RGImage atlasImage{};
		RGImage atlasDepthImage{};
		std::uint32_t atlasBindlessSlot = UINT32_MAX;
	};

	struct LightBuffersProduct
	{
		RGBuffer lights{};
		RGBuffer tileHeaders{};
		RGBuffer tileIndices{};
	};

	template<typename T>
	struct FrameProductTraits
	{
		static void Validate(std::string_view /*name*/, const T& /*product*/, const FrameBlackboard::ProductMetadata& /*metadata*/, std::vector<std::string>& /*warnings*/)
		{
		}
	};

	namespace render_graph_detail
	{
		[[nodiscard]] inline bool IsZeroExtent(gpu::Extent2D extent) noexcept
		{
			return extent.width == 0 || extent.height == 0;
		}

		[[nodiscard]] inline bool ExtentMismatch(gpu::Extent2D lhs, gpu::Extent2D rhs) noexcept
		{
			return lhs.width != rhs.width || lhs.height != rhs.height;
		}

		inline void ValidateImageProduct(
		        std::string_view name, std::string_view label, RGImage image, gpu::Extent2D extent, gpu::Format format, std::uint32_t bindlessSlot, const FrameBlackboard::ProductMetadata& metadata, std::vector<std::string>& warnings)
		{
			if (!image.IsValid())
			{
				warnings.push_back(std::format("frame product '{}:{}' has no valid image handle.", label, name));
			}
			if (IsZeroExtent(extent))
			{
				warnings.push_back(std::format("frame product '{}:{}' has zero extent.", label, name));
			}
			if (format == gpu::Format::Undefined)
			{
				warnings.push_back(std::format("frame product '{}:{}' has undefined format.", label, name));
			}
			if (bindlessSlot == UINT32_MAX)
			{
				warnings.push_back(std::format("frame product '{}:{}' has no bindless sampled slot.", label, name));
			}
			if (metadata.extent.has_value() && ExtentMismatch(*metadata.extent, extent))
			{
				warnings.push_back(std::format("frame product '{}:{}' metadata extent does not match product extent.", label, name));
			}
			if (metadata.format != gpu::Format::Undefined && metadata.format != format)
			{
				warnings.push_back(std::format("frame product '{}:{}' metadata format does not match product format.", label, name));
			}
			if (metadata.bindlessSlot != UINT32_MAX && metadata.bindlessSlot != bindlessSlot)
			{
				warnings.push_back(std::format("frame product '{}:{}' metadata bindless slot does not match product bindless slot.", label, name));
			}
		}
	} // namespace render_graph_detail

	template<>
	struct FrameProductTraits<MainViewProduct>
	{
		static void Validate(std::string_view name, const MainViewProduct& product, const FrameBlackboard::ProductMetadata& metadata, std::vector<std::string>& warnings)
		{
			if (render_graph_detail::IsZeroExtent(product.extent))
			{
				warnings.push_back(std::format("frame product 'MainViewProduct:{}' has zero extent.", name));
			}
			if (product.frameConstantsAddr == 0)
			{
				warnings.push_back(std::format("frame product 'MainViewProduct:{}' has no frame constants address.", name));
			}
			if (metadata.extent.has_value() && render_graph_detail::ExtentMismatch(*metadata.extent, product.extent))
			{
				warnings.push_back(std::format("frame product 'MainViewProduct:{}' metadata extent does not match product extent.", name));
			}
			if (metadata.frameSlot != UINT32_MAX && metadata.frameSlot != product.frameSlot)
			{
				warnings.push_back(std::format("frame product 'MainViewProduct:{}' metadata frame slot does not match product frame slot.", name));
			}
		}
	};

	template<>
	struct FrameProductTraits<SceneDepthProduct>
	{
		static void Validate(std::string_view name, const SceneDepthProduct& product, const FrameBlackboard::ProductMetadata& metadata, std::vector<std::string>& warnings)
		{
			render_graph_detail::ValidateImageProduct(name, "SceneDepthProduct", product.image, product.extent, product.format, product.bindlessSlot, metadata, warnings);
		}
	};

	template<>
	struct FrameProductTraits<HdrColorProduct>
	{
		static void Validate(std::string_view name, const HdrColorProduct& product, const FrameBlackboard::ProductMetadata& metadata, std::vector<std::string>& warnings)
		{
			render_graph_detail::ValidateImageProduct(name, "HdrColorProduct", product.image, product.extent, product.format, product.bindlessSlot, metadata, warnings);
		}
	};

	template<>
	struct FrameProductTraits<GtaoProduct>
	{
		static void Validate(std::string_view name, const GtaoProduct& product, const FrameBlackboard::ProductMetadata& metadata, std::vector<std::string>& warnings)
		{
			render_graph_detail::ValidateImageProduct(name, "GtaoProduct", product.image, product.extent, product.format, product.bindlessSlot, metadata, warnings);
		}
	};

	template<>
	struct FrameProductTraits<DirectionalShadowProduct>
	{
		static void Validate(std::string_view name, const DirectionalShadowProduct& product, const FrameBlackboard::ProductMetadata& /*metadata*/, std::vector<std::string>& warnings)
		{
			if (product.depthImages.empty())
			{
				warnings.push_back(std::format("frame product 'DirectionalShadowProduct:{}' has no cascade images.", name));
			}
			if (product.depthImages.size() != product.bindlessSlots.size())
			{
				warnings.push_back(std::format("frame product 'DirectionalShadowProduct:{}' image count does not match bindless slot count.", name));
			}
			for (std::size_t index = 0; index < product.depthImages.size(); ++index)
			{
				if (!product.depthImages[index].IsValid())
				{
					warnings.push_back(std::format("frame product 'DirectionalShadowProduct:{}' cascade {} has no valid image handle.", name, index));
				}
				if (index >= product.bindlessSlots.size() || product.bindlessSlots[index] == UINT32_MAX)
				{
					warnings.push_back(std::format("frame product 'DirectionalShadowProduct:{}' cascade {} has no bindless sampled slot.", name, index));
				}
			}
		}
	};

	template<>
	struct FrameProductTraits<LocalShadowProduct>
	{
		static void Validate(std::string_view name, const LocalShadowProduct& product, const FrameBlackboard::ProductMetadata& /*metadata*/, std::vector<std::string>& warnings)
		{
			if (!product.atlasImage.IsValid())
			{
				warnings.push_back(std::format("frame product 'LocalShadowProduct:{}' has no valid atlas image.", name));
			}
			if (!product.atlasDepthImage.IsValid())
			{
				warnings.push_back(std::format("frame product 'LocalShadowProduct:{}' has no valid atlas depth image.", name));
			}
			if (product.atlasBindlessSlot == UINT32_MAX)
			{
				warnings.push_back(std::format("frame product 'LocalShadowProduct:{}' has no bindless atlas slot.", name));
			}
		}
	};

	template<>
	struct FrameProductTraits<LightBuffersProduct>
	{
		static void Validate(std::string_view name, const LightBuffersProduct& product, const FrameBlackboard::ProductMetadata& /*metadata*/, std::vector<std::string>& warnings)
		{
			if (!product.lights.IsValid())
			{
				warnings.push_back(std::format("frame product 'LightBuffersProduct:{}' has no valid lights buffer.", name));
			}
			if (!product.tileHeaders.IsValid())
			{
				warnings.push_back(std::format("frame product 'LightBuffersProduct:{}' has no valid tile header buffer.", name));
			}
			if (!product.tileIndices.IsValid())
			{
				warnings.push_back(std::format("frame product 'LightBuffersProduct:{}' has no valid tile index buffer.", name));
			}
		}
	};

	// Backward-compatible shims. Prefer gpu::ClearColor / gpu::ClearDepth.
	[[nodiscard]] inline gpu::ClearValue ClearColorValue(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept
	{
		return gpu::ClearColor(r, g, b, a);
	}

	[[nodiscard]] inline gpu::ClearValue ClearDepthValue(float depth = 1.0f, uint32_t stencil = 0u) noexcept
	{
		return gpu::ClearDepth(depth, stencil);
	}

	struct FrameResourceContext
	{
		FrameTarget target{};
		gpu::Extent2D extent{};
		std::uint64_t frameIndex = 0;
		std::uint32_t frameSlot = 0;
		std::uint32_t swapchainImageIndex = UINT32_MAX;
		std::uint64_t frameConstantsAddr = 0;
	};

	// Data made available inside pass execute callbacks.
	struct PassContext
	{
		gpu::CommandList& recorder;
		const FrameResourceContext& frame;
		gpu::Extent2D extent;
		std::uint64_t frameConstantsAddr = 0;
		std::uint32_t frameIndex = 0;
		std::uint32_t frameSlot = 0;
	};
} // namespace aether
