#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class DiagnosticEngine;
	class GraphicsPipeline;
} // namespace aether

namespace aether::gpu
{
	// by GpuDevice or AsyncComputeContext, never stored.
	class CommandList
	{
	public:
		CommandList() = default;
		~CommandList() = default;

		explicit CommandList(void* commandBuffer) noexcept
		      : m_cmd(commandBuffer)
		{
		}

		[[nodiscard]] bool IsValid() const noexcept
		{
			return m_cmd != nullptr;
		}

		[[nodiscard]] CommandList View() const noexcept
		{
			CommandList out{};
			out.m_cmd = m_cmd;
			return out;
		}

		[[nodiscard]] gpu::CommandBuffer GetCommandBuffer() const noexcept
		{
			return m_cmd;
		}

		void BindPipeline(PipelineView pipeline) noexcept;
		void BindPipeline(GraphicsPipeline& pipeline);

		void BindComputePipeline(PipelineView pipeline) noexcept;

		void BindIndexBuffer(void* buffer, DeviceAddress offset = 0, IndexType indexType = IndexType::U32) noexcept;
		void BindIndexBuffer(BufferHandle buffer, DeviceAddress offset = 0, IndexType indexType = IndexType::U32) noexcept;

		void BindVertexBuffer(void* buffer, DeviceAddress offset = 0) noexcept;

		void SetLineWidth(float lineWidth) noexcept;
		void SetCullMode(CullMode cullMode) noexcept;

		void Draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0, std::uint32_t firstInstance = 0);
		void DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0, std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);
		void Dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ);

		void DrawIndirect(void* buffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride);
		void DrawIndexedIndirect(void* buffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride);
		void DrawIndexedIndirectCount(void* indirectBuffer, DeviceAddress indirectOffset, void* countBuffer, DeviceAddress countOffset, std::uint32_t maxDrawCount, std::uint32_t stride);

		void FillBuffer(void* buffer, DeviceAddress offset, DeviceAddress size, std::uint32_t value) noexcept;

		void SetViewport(const Viewport& viewport);
		void SetViewport(std::uint32_t firstViewport, std::span<const Viewport> viewports);
		void SetScissor(const Rect2D& scissor);
		void SetScissor(std::uint32_t firstScissor, std::span<const Rect2D> scissors);

		void PushDataRaw(std::uint32_t offset, std::span<const std::byte> data);

		void BeginDebugLabel(std::string_view name, float r = 0.15f, float g = 0.55f, float b = 0.90f, float a = 1.0f);
		void EndDebugLabel();

		void PipelineMemoryBarrier(PipelineStage srcStage, AccessFlags srcAccess, PipelineStage dstStage, AccessFlags dstAccess) noexcept;
		void ImageMemoryBarrier(void* image, ImageLayout oldLayout, ImageLayout newLayout, ImageAspect aspect, PipelineStage srcStage, AccessFlags srcAccess, PipelineStage dstStage, AccessFlags dstAccess) noexcept;

		void BeginRendering(const gpu::RenderingInfo& info);
		void EndRendering();

		void WriteTimestamp(void* queryPool, std::uint32_t slot, PipelineStage stage) noexcept;
		// Command-buffer reset rather than the host-side vkResetQueryPool, which would
		// pull in the hostQueryReset feature for no benefit.
		void ResetQueryPool(void* queryPool, std::uint32_t firstSlot, std::uint32_t slotCount) noexcept;

		void CopyBuffer(void* src, void* dst, std::uint64_t srcOffset, std::uint64_t dstOffset, std::uint64_t size) noexcept;
		void CopyImageToBuffer(void* srcImage,
		        void* dstBuffer,
		        gpu::ImageLayout srcImageLayout,
		        gpu::ImageAspect aspect,
		        std::uint32_t width,
		        std::uint32_t height,
		        std::uint64_t bufferOffset = 0,
		        std::int32_t imageOffsetX = 0,
		        std::int32_t imageOffsetY = 0) noexcept;
		void CopyBufferToImage(void* srcBuffer,
		        void* dstImage,
		        gpu::ImageLayout dstImageLayout,
		        gpu::ImageAspect aspect,
		        std::uint32_t width,
		        std::uint32_t height,
		        std::uint64_t bufferOffset = 0,
		        std::int32_t imageOffsetX = 0,
		        std::int32_t imageOffsetY = 0) noexcept;

		static void SetDebugLabelFunctions(void* beginFn, void* endFn) noexcept;
		static void SetDiagnosticEngine(DiagnosticEngine* engine) noexcept;
		static void SetAlphaToOneDynamicStateSupported(bool supported) noexcept;

	private:
		void* m_cmd = nullptr;
	};
} // namespace aether::gpu
