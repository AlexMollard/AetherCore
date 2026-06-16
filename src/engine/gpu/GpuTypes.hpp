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

	struct GpuExtent2D
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		GpuExtent2D() = default;

		GpuExtent2D(std::uint32_t w, std::uint32_t h)
		      : width(w), height(h)
		{
		}

		template<typename Extent2D>
		explicit GpuExtent2D(const Extent2D& ext)
		      : width(ext.width), height(ext.height)
		{
		}
	};
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
	using DescriptorSetLayout = void*;
	using DescriptorPool = void*;
	using Pipeline = void*;
	using PipelineLayout = void*;
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

	// Engine-facing descriptor buffer info for push-descriptor writes.
	// Mirrors the layout of VkDescriptorBufferInfo. The `buffer` field is
	// the raw VkBuffer (void*); the backend translates via reinterpret_cast
	// in the same way the existing CommandList binding methods do. This
	// keeps the header Vulkan-free while letting callers build descriptor
	// write payloads from local buffer-table sources.
	struct GpuDescriptorBufferInfo
	{
		Buffer buffer = nullptr;
		DeviceAddress offset = 0;
		DeviceAddress range = 0;
	};

	// Engine-facing descriptor image info for push-descriptor writes.
	// Mirrors VkDescriptorImageInfo. `sampler` may be nullptr for
	// STORAGE_IMAGE / SAMPLED_IMAGE writes. The backend translates the
	// void* to VkImageView / VkSampler at the seam in CommandList.
	// Forward-declared as opaque pointers so the struct can appear
	// before the `using` aliases below.
	struct GpuDescriptorImageInfo
	{
		void* sampler = nullptr;
		void* imageView = nullptr;
		ImageLayout imageLayout = ImageLayout::Undefined;
	};

	// Engine-facing push-descriptor write payload. Supports storage-buffer,
	// storage-image, and combined-image-sampler paths today; other types
	// (uniform buffer, texel buffer) are added when the engine needs them.
	// Mirrors the layout of VkWriteDescriptorSet; the backend translates
	// one-for-one in CommandList. Exactly one of `bufferInfo` or `imageInfo`
	// is set based on `descriptorType`.
	struct GpuWriteDescriptorSet
	{
		std::uint32_t dstBinding = 0;
		std::uint32_t descriptorCount = 0;
		DescriptorType descriptorType = DescriptorType::StorageBuffer;
		const GpuDescriptorBufferInfo* bufferInfo = nullptr;
		const GpuDescriptorImageInfo* imageInfo = nullptr;
	};

	// One binding inside a descriptor-set layout. Mirrors the subset of
	// VkDescriptorSetLayoutBinding the engine needs to declare storage-
	// buffer bindings.
	struct GpuDescriptorSetLayoutBinding
	{
		std::uint32_t binding = 0;
		DescriptorType descriptorType = DescriptorType::StorageBuffer;
		std::uint32_t descriptorCount = 1;
		ShaderStage stageFlags = ShaderStage::None;
	};

	// Push-constant range for a pipeline layout. Mirrors
	// VkPushConstantRange.
	struct PushConstantRange
	{
		ShaderStage stageFlags = ShaderStage::None;
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
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
