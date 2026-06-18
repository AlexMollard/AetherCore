#pragma once

#include <cstdint>
#include <span>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"

namespace aether
{
	// aether::GpuFormat is a short-form alias for aether::gpu::Format -
	// see GpuFormat.hpp for the full format enum.

	inline constexpr std::uint32_t kMaxFramesInFlight = 3;
} // namespace aether

namespace aether::gpu
{
	using DeviceAddress = std::uint64_t;
	using DeviceSize = std::uint64_t;

	// Opaque handle aliases. Borrowed primitives (lifetime managed by the
	// facade) live as `void*` typedefs here. Owned resources use the
	// generation-checked typed handles from `gpu/GpuHandles.hpp`. The full
	// borrow-vs-owned table lives in docs/plans/gpu-abstraction-rendering-audit.md.
	using DescriptorSet = void*;
	using Pipeline = void*;
	using PipelineCache = void*;
	using Device = void*;
	using PhysicalDevice = void*;
	using Allocator = void*;
	using CommandPool = void*;
	using Queue = void*;
	using CommandBuffer = void*;
	using Image = void*;
	using ImageView = void*;
	using Buffer = void*;
	using Event = void*;
	using Sampler = void*;
	using QueryPool = void*;
	using Fence = void*;

	// Viewport state for the dynamic state path. Depth range is [0, 1].
	struct Viewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		float minDepth = 0.0f;
		float maxDepth = 1.0f;
	};

	// Inclusive/exclusive 2D integer rectangle used for scissor state.
	// Mirrors the layout of VkRect2D offset+extent.
	struct Rect2D
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	// Indirect-draw command struct mirror. Mirrors the layout of
	// VkDrawIndexedIndirectCommand (5 * uint32_t). Defined in the gpu/
	// facade so engine code can take sizeof() of it without including
	// vulkan/volk.hpp. The backend writes the same bytes to the GPU.
	struct DrawIndexedIndirectCommand
	{
		std::uint32_t indexCount = 0;
		std::uint32_t instanceCount = 0;
		std::uint32_t firstIndex = 0;
		std::int32_t vertexOffset = 0;
		std::uint32_t firstInstance = 0;
	};

	static_assert(sizeof(DrawIndexedIndirectCommand) == 20, "DrawIndexedIndirectCommand must match VkDrawIndexedIndirectCommand layout");

	// Indirect-draw command struct mirror (non-indexed). Mirrors
	// VkDrawIndirectCommand (4 * uint32_t).
	struct DrawIndirectCommand
	{
		std::uint32_t vertexCount = 0;
		std::uint32_t instanceCount = 0;
		std::uint32_t firstVertex = 0;
		std::uint32_t firstInstance = 0;
	};

	static_assert(sizeof(DrawIndirectCommand) == 16, "DrawIndirectCommand must match VkDrawIndirectCommand layout");

	// -------------------------------------------------------------------------
	// Image + buffer barriers
	// -------------------------------------------------------------------------
	// Image/buffer memory barriers. The `image` / `buffer` fields are
	// opaque engine-side handles resolved by the storage; layout and access
	// values are the engine-side enums from GpuEnums.hpp.

	struct ImageMemoryBarrier
	{
		Image image = nullptr;
		ImageLayout oldLayout = ImageLayout::Undefined;
		ImageLayout newLayout = ImageLayout::Undefined;
		ImageAspect aspect = ImageAspect::Color;
		std::uint32_t baseMipLevel = 0;
		std::uint32_t levelCount = 1;
		std::uint32_t baseArrayLayer = 0;
		std::uint32_t layerCount = 1;
		PipelineStage srcStage = PipelineStage::None;
		AccessFlags srcAccess = AccessFlags::None;
		PipelineStage dstStage = PipelineStage::None;
		AccessFlags dstAccess = AccessFlags::None;
	};

	struct BufferMemoryBarrier
	{
		Buffer buffer = nullptr;
		DeviceSize offset = 0;
		DeviceSize size = static_cast<DeviceSize>(-1); // VK_WHOLE_SIZE
		PipelineStage srcStage = PipelineStage::None;
		AccessFlags srcAccess = AccessFlags::None;
		PipelineStage dstStage = PipelineStage::None;
		AccessFlags dstAccess = AccessFlags::None;
	};

	// -------------------------------------------------------------------------
	// Dynamic rendering
	// -------------------------------------------------------------------------
	// Mirrors of VkRenderingAttachmentInfo / VkRenderingInfo. Used by
	// RenderGraph::Execute to describe the per-pass color / depth attachments
	// and the render area. The backend (gpu/CommandList.cpp) translates to
	// Dynamic rendering.

	struct RenderingAttachmentInfo
	{
		ImageView imageView = nullptr;
		ImageLayout imageLayout = ImageLayout::Undefined;
		LoadOp loadOp = LoadOp::Load;
		StoreOp storeOp = StoreOp::Store;
		ClearValue clearValue{};
	};

	struct RenderingInfo
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t layerCount = 1;
		std::span<const RenderingAttachmentInfo> colorAttachments;
		const RenderingAttachmentInfo* depthAttachment = nullptr;
	};
} // namespace aether::gpu
