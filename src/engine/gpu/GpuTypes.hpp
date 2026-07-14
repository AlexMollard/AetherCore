#pragma once

#include <cstdint>
#include <span>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"

namespace aether
{

	inline constexpr std::uint32_t kMaxFramesInFlight = 3;
}

namespace aether::gpu
{
	using DeviceAddress = std::uint64_t;
	using DeviceSize = std::uint64_t;

	// Opaque handle aliases. Borrowed primitives (lifetime managed by the
	using DescriptorSet = void*;
	using Pipeline = void*;
	using PipelineView = const void*;
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

	struct Viewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		float minDepth = 0.0f;
		float maxDepth = 1.0f;
	};

	// Mirrors the layout of VkRect2D offset+extent.
	struct Rect2D
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	// Indirect-draw command struct mirror. Mirrors the layout of
	struct DrawIndexedIndirectCommand
	{
		std::uint32_t indexCount = 0;
		std::uint32_t instanceCount = 0;
		std::uint32_t firstIndex = 0;
		std::int32_t vertexOffset = 0;
		std::uint32_t firstInstance = 0;
	};

	static_assert(sizeof(DrawIndexedIndirectCommand) == 20, "DrawIndexedIndirectCommand must match VkDrawIndexedIndirectCommand layout");

	struct DrawIndirectCommand
	{
		std::uint32_t vertexCount = 0;
		std::uint32_t instanceCount = 0;
		std::uint32_t firstVertex = 0;
		std::uint32_t firstInstance = 0;
	};

	static_assert(sizeof(DrawIndirectCommand) == 16, "DrawIndirectCommand must match VkDrawIndirectCommand layout");

	// opaque engine-side handles resolved by the storage; layout and access

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
		DeviceSize size = static_cast<DeviceSize>(-1);
		PipelineStage srcStage = PipelineStage::None;
		AccessFlags srcAccess = AccessFlags::None;
		PipelineStage dstStage = PipelineStage::None;
		AccessFlags dstAccess = AccessFlags::None;
	};

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
