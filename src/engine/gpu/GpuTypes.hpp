#pragma once

#include <cstdint>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"

namespace aether
{
	// aether::GpuFormat is provided as a backward-compatible alias for
	// aether::gpu::Format - see GpuFormat.hpp. New code should use
	// aether::gpu::Format directly; the old 6-value enum that previously lived
	// here has been superseded by the full enum in GpuFormat.hpp.

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

	// Viewport state (mirrors VkViewport for the dynamic state path). Floats
	// are used directly because depth/normalisation semantics are the same
	// on every backend the engine supports.
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
	// write payloads from local UniqueBuffer / buffer-table sources.
	struct GpuDescriptorBufferInfo
	{
		void* buffer = nullptr;
		DeviceAddress offset = 0;
		DeviceAddress range = 0;
	};

	// Engine-facing push-descriptor write payload. Only the storage-buffer
	// path is supported today (DescriptorType::StorageBuffer); the image
	// variants are added when the engine needs them. Mirrors the layout of
	// VkWriteDescriptorSet; the backend translates one-for-one in CommandList.
	struct GpuWriteDescriptorSet
	{
		std::uint32_t dstBinding = 0;
		std::uint32_t descriptorCount = 0;
		DescriptorType descriptorType = DescriptorType::StorageBuffer;
		const GpuDescriptorBufferInfo* bufferInfo = nullptr;
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

	// Opaque engine-facing aliases for opaque Vulkan handles. The CommandList
	// API takes these directly so the call site never mentions Vk*. The
	// backend (vulkan/) defines the same names as their real Vk* types so
	// the implementations can convert with a single static_cast.
	using DescriptorSet = void*;
	using PipelineLayout = void*;
} // namespace aether::gpu
