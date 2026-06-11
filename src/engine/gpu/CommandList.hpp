#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class GraphicsPipeline;
}

namespace aether::gpu
{
	// ─────────────────────────────────────────────────────────────────────────
	// CommandList - Phase 3 of the GPU refactor
	// ─────────────────────────────────────────────────────────────────────────
	// Engine-facing replacement for the raw VkCommandBuffer work in
	// rendering/ (CommandRecorder has been removed). This header is Vulkan-free: every Vk* type
	// stays in gpu/CommandList.cpp. Passes hold a CommandList& and call
	// methods that take engine handles (BufferHandle, PipelineHandle) or
	// device-address integers (gpu::DeviceAddress), never raw Vk*.
	//
	// Construction: a CommandList is created by GpuDevice::GetCurrentCommandList
	// wrapping the swapchain's current VkCommandBuffer, or by
	// AsyncComputeContext::GetCommandList wrapping an async-compute VkCommandBuffer.
	// Both forms live for the duration of one frame and are not stored across frames.
	//
	// Phase 3 is incremental: this type is added additively, passes are migrated
	// one at a time. The first migration (SkyboxPass) uses
	// BindPipeline(VkPipeline) / PushConstantsRaw(VkPipelineLayout) overloads
	// that take the raw Vk* void*s but resolve them inside CommandList.cpp.
	// The end-state API will take engine handles (PipelineHandle, etc.) and
	// look them up in the ResourceRegistry; that arrives with the GraphicsPipeline
	// migration in Phase 5.
	//
	// Barriers (vkCmdPipelineBarrier2) belong in RenderGraph (Phase 5), not here,
	// so this type deliberately does not expose a barrier method.
	class CommandList
	{
	public:
		CommandList() = default;
		~CommandList() = default;

		// Wraps a real VkCommandBuffer. Construction is backend-only - engine
		// code gets a CommandList from GpuDevice / AsyncComputeContext.
		// The void* parameter is a non-owning pointer; lifetime is managed by
		// Swapchain / AsyncComputeContext, not by this type.
		explicit CommandList(void* vkCommandBuffer) noexcept
		      : m_cmd(vkCommandBuffer)
		{
		}

		[[nodiscard]] bool IsValid() const noexcept
		{
			return m_cmd != nullptr;
		}

		// Returns the raw Vulkan command-buffer handle. Backend-only - engine
		// code should not need to use this. RenderGraph::Execute() needs it to
		// pass into vkutil helpers + Tracy GPU zones.
		[[nodiscard]] void* GetCommandBuffer() const noexcept
		{
			return m_cmd;
		}

		// Bind a graphics pipeline. The first overload takes a raw VkPipeline
		// void* for the first-migration slice; the second (handle-based) is
		// the end-state API and is the only one new code should use; the
		// third takes a rendering::GraphicsPipeline& for the transitional
		// slice used by QuadRenderer and similar UI passes. All three store
		// the pipeline layout internally for the next PushConstantsRaw call
		// (matches the existing SkyboxPass pattern of bind-then-push).
		void BindPipeline(void* vkPipeline, void* vkPipelineLayout) noexcept;
		void BindPipeline(PipelineHandle pipeline);
		void BindPipeline(GraphicsPipeline& pipeline);

		// Bind a compute pipeline. Stores the layout for the layout-less
		// PushConstantsRaw overload.
		void BindComputePipeline(void* vkPipeline, void* vkPipelineLayout) noexcept;

		// Bind a descriptor set to a pipeline layout. The first overload
		// takes a raw VkPipelineLayout void* for the first-migration slice;
		// the second uses the layout bound by the most recent BindPipeline
		// call (matches the bind-then-push pattern). Both are pure bind
		// helpers; vkDescriptorSet is the raw VkDescriptorSet void*.
		void BindDescriptorSet(PipelineLayout pipelineLayout, std::uint32_t set, DescriptorSet descriptorSet, std::uint32_t dynamicOffsetCount = 0, const std::uint32_t* dynamicOffsets = nullptr) noexcept;
		void BindDescriptorSet(std::uint32_t set, DescriptorSet descriptorSet, std::uint32_t dynamicOffsetCount = 0, const std::uint32_t* dynamicOffsets = nullptr) noexcept;

		// Bind an index buffer. indexType is the engine-side IndexType
		// (U16 / U32) - the impl maps it to VkIndexType.
		void BindIndexBuffer(void* vkBuffer, DeviceAddress offset = 0, IndexType indexType = IndexType::U32) noexcept;

		// Bind a vertex buffer at a device-address offset. Matches
		// vkCmdBindVertexBuffers(firstBinding=0, ...).
		void BindVertexBuffer(void* vkBuffer, DeviceAddress offset = 0) noexcept;

		// Set dynamic line width (vkCmdSetLineWidth). Most engine passes leave
		// this at the 1.0 default; debug line passes (PhysicsDebugRenderer)
		// use 2.0.
		void SetLineWidth(float lineWidth) noexcept;

		// Draw commands. firstVertex / firstInstance default to 0; matches
		// the vkCmdDraw signature so migration is drop-in.
		void Draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0, std::uint32_t firstInstance = 0);

		// Indexed draw with a bound index buffer (BindIndexBuffer is implied
		// for the call - the engine is expected to have bound the IB first).
		void DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0, std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);

		// Dispatch a compute shader. groupCountX/Y/Z match vkCmdDispatch.
		void Dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ);

		// Draw commands driven by GPU-side indirect buffers. Used by the
		// cull/forward chain: the cull compute pass writes VkDrawIndexed-
		// IndirectCommand entries, the forward pass consumes them. The
		// *Count variants read the actual draw count from a GPU buffer.
		void DrawIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride);
		void DrawIndexedIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride);
		void DrawIndexedIndirectCount(void* vkIndirectBuffer, DeviceAddress indirectOffset, void* vkCountBuffer, DeviceAddress countOffset, std::uint32_t maxDrawCount, std::uint32_t stride);

		// Push a tightly-packed set of descriptor writes into a push-descriptor
		// set (KHR_push_descriptor). The writes are passed as raw VkWrite-
		// DescriptorSet byte blobs so the engine can build them with their
		// own helper; the layout is taken from the most recent BindPipeline
		// call. This is what the tiled-forward lighting pass uses to update
		// the per-view light buffer / tile-header / tile-index sets without
		// round-tripping the descriptor pool. The *writes* pointer is the
		// raw VkWriteDescriptorSet array; the *layout* parameter is the raw
		// VkDescriptorSetLayout void*. This is intentionally raw-void* in
		// the first slice (Phase 5 migrates it to engine handles).
		//
		// Two overloads: the explicit-bind-point variant is the escape hatch
		// for the rare case where no pipeline has been bound yet. The cached
		// overload uses the most recent BindPipeline / BindComputePipeline
		// call's bind point, eliminating the "graphics push on a compute
		// command buffer" class of bug at the call site - which is exactly
		// the validation error that motivated this. The validation layer
		// still enforces VUID-vkCmdPushDescriptorSet-pipelineBindPoint-00363
		// so a wrong cached value fails fast with a clear error.
		void PushDescriptorSet(PipelineLayout pipelineLayout, std::uint32_t set, std::uint32_t writeCount, const void* vkWriteDescriptorSets) noexcept;
		void PushDescriptorSet(PipelineBindPoint bindPoint, PipelineLayout pipelineLayout, std::uint32_t set, std::uint32_t writeCount, const void* vkWriteDescriptorSets) noexcept;

		// Engine-types push descriptor write overload. The translate from
		// GpuWriteDescriptorSet to VkWriteDescriptorSet happens here in the
		// .cpp. The GpuDescriptorBufferInfo array pointed to by each write
		// must remain valid for the duration of this call (callers
		// typically construct it locally for the call).
		void PushDescriptorSet(PipelineLayout pipelineLayout, std::uint32_t set, std::span<const GpuWriteDescriptorSet> writes) noexcept;
		void PushDescriptorSet(PipelineBindPoint bindPoint, PipelineLayout pipelineLayout, std::uint32_t set, std::span<const GpuWriteDescriptorSet> writes) noexcept;

		// Fill a buffer region with a 4-byte value. Used by RenderQueue to
		// clear per-frame animation/skin-palette slots on first use. The
		// buffer is the raw VkBuffer void*. Mirrors vkCmdFillBuffer.
		void FillBuffer(void* vkBuffer, DeviceAddress offset, DeviceAddress size, std::uint32_t value) noexcept;

		// Dynamic state - viewport + scissor. Both apply to the first
		// viewport/scissor slot. Most engine passes set these once per
		// pass; the first-viewport-first-scissor pattern matches the
		// current fullscreen-triangle post passes.
		void SetViewport(const Viewport& viewport);
		void SetViewport(std::uint32_t firstViewport, std::span<const Viewport> viewports);
		void SetScissor(const Rect2D& scissor);
		void SetScissor(std::uint32_t firstScissor, std::span<const Rect2D> scissors);

		// Push a tightly-packed blob of constants. The first overload takes
		// a raw VkPipelineLayout void* for the first-migration slice; the
		// second uses the layout bound by the most recent BindPipeline call.
		// The engine uses this with sizeof(uint64_t) = an 8-byte BDA.
		void PushConstantsRaw(void* vkPipelineLayout, ShaderStage stages, std::uint32_t offset, std::span<const std::byte> data);
		void PushConstantsRaw(ShaderStage stages, std::uint32_t offset, std::span<const std::byte> data);

		// Debug labels - RenderDoc / validation scope markers.
		void BeginDebugLabel(std::string_view name, float r = 0.15f, float g = 0.55f, float b = 0.90f, float a = 1.0f);
		void EndDebugLabel();

		// Insert a global memory barrier (vkCmdPipelineBarrier2 with
		// VkMemoryBarrier2, no image/buffer barrier entries). This is the
		// only barrier kind the engine issues today: host->compute,
		// compute->compute, compute->fragment. The Phase 5 RenderGraph
		// will own barrier insertion end-to-end; this method exists so
		// the few sites that need an inline barrier (LightingManager,
		// AsyncComputeContext) can use the Vulkan-free CommandList API.
		// Stage and access values are the engine-side bit flags defined
		// in GpuEnums.hpp; the .cpp translates to VkPipelineStageFlags2 /
		// VkAccessFlags2 via GpuEnumConversions.
		void PipelineMemoryBarrier(PipelineStage srcStage, AccessFlags srcAccess, PipelineStage dstStage, AccessFlags dstAccess) noexcept;

		// Begin/end dynamic rendering (vkCmdBeginRendering / vkCmdEndRendering).
		// The transitional overload takes a raw VkRenderingInfo* as void*;
		// the final overload will take engine-side types once RenderGraph
		// internal storage migrates (P5(d)).
		void BeginRendering(const void* vkRenderingInfo);
		void EndRendering();

		// Write a GPU timestamp (vkCmdWriteTimestamp2). queryPool is an opaque
		// pointer to a VkQueryPool in the implementation.
		void WriteTimestamp(void* queryPool, std::uint32_t slot, PipelineStage stage) noexcept;

		// Copy region from one buffer to another (vkCmdCopyBuffer).
		// src/dst are opaque pointers to VkBuffer in the implementation.
		void CopyBuffer(void* src, void* dst, std::uint64_t srcOffset, std::uint64_t dstOffset, std::uint64_t size) noexcept;

		// Wire the debug-label function pointers used by BeginDebugLabel /
		// EndDebugLabel. Called by the backend (VulkanContext) at engine init
		// with the addresses of vkCmdBeginDebugUtilsLabelEXT and
		// vkCmdEndDebugUtilsLabelEXT. Passing null disables debug labels.
		// The void* types keep the header Vulkan-free; the impl casts to the
		// real PFN_vkCmd*DebugUtilsLabelEXT types.
		static void SetDebugLabelFunctions(void* beginFn, void* endFn) noexcept;

	private:
		// Opaque pointer to a VkCommandBuffer. The .cpp sees through it; the
		// header does not.
		void* m_cmd = nullptr;
		// The most recently bound VkPipelineLayout. Set by BindPipeline; used
		// by the layout-less PushConstantsRaw overload. Also opaque (a void*
		// to a VkPipelineLayout in the impl). Cached here so callers can do
		// "bind then push" without re-passing the layout.
		void* m_boundLayout = nullptr;
		// The bind point of the most recently bound pipeline. Set by
		// BindPipeline (Graphics) and BindComputePipeline (Compute). Read by
		// the cached PushDescriptorSet overload so it can pick the right
		// pipelineBindPoint without the caller having to remember.
		PipelineBindPoint m_boundBindPoint = PipelineBindPoint::Graphics;
	};
} // namespace aether::gpu
