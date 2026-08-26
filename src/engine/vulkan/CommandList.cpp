#include "gpu/CommandList.hpp"

#include <format>
#include <utility>
#include <vector>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "vulkan/DiagnosticEngine.hpp"
#include "vulkan/GlobalBindingLayout.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/volk.hpp"

namespace aether::gpu
{
	namespace
	{
		inline VkCommandBuffer AsVkCmd(void* p) noexcept
		{
			return static_cast<VkCommandBuffer>(p);
		}

		inline VkBuffer AsVkBuffer(void* p) noexcept
		{
			return static_cast<VkBuffer>(p);
		}

		inline VkViewport ToVkViewport(const Viewport& v) noexcept
		{
			return VkViewport{
			        .x = v.x,
			        .y = v.y,
			        .width = v.width,
			        .height = v.height,
			        .minDepth = v.minDepth,
			        .maxDepth = v.maxDepth,
			};
		}

		inline VkRect2D ToVkRect2D(const Rect2D& r) noexcept
		{
			return VkRect2D{
			        .offset = {.x = r.x, .y = r.y},
			        .extent = {.width = r.width, .height = r.height},
			};
		}

		inline VkIndexType ToVkIndexType(IndexType t) noexcept
		{
			return (t == IndexType::U16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		}

		PFN_vkCmdBeginDebugUtilsLabelEXT s_beginDebugLabel = nullptr;
		PFN_vkCmdEndDebugUtilsLabelEXT s_endDebugLabel = nullptr;
		DiagnosticEngine* s_diagnosticEngine = nullptr;
		bool s_alphaToOneDynamicStateSupported = true;

		void RecordDiagnosticEvent(std::string_view message)
		{
			if (s_diagnosticEngine != nullptr)
			{
				s_diagnosticEngine->RecordEvent(message);
			}
		}

		template<typename... Args>
		void RecordDiagnosticEventFmt(std::format_string<Args...> fmt, Args&&... args)
		{
			if (s_diagnosticEngine != nullptr)
			{
				s_diagnosticEngine->RecordEvent(std::format(fmt, std::forward<Args>(args)...));
			}
		}
	} // namespace

	void CommandList::SetDebugLabelFunctions(void* beginFn, void* endFn) noexcept
	{
		s_beginDebugLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(beginFn);
		s_endDebugLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(endFn);
	}

	void CommandList::SetDiagnosticEngine(DiagnosticEngine* engine) noexcept
	{
		s_diagnosticEngine = engine;
	}

	void CommandList::SetAlphaToOneDynamicStateSupported(const bool supported) noexcept
	{
		s_alphaToOneDynamicStateSupported = supported;
	}

	void CommandList::BindPipeline(PipelineView pipeline) noexcept
	{
		if (m_cmd == nullptr || pipeline == nullptr)
		{
			return;
		}
		const auto* entry = static_cast<const ::aether::ResourceRegistry::PipelineEntry*>(pipeline);
		const VkCommandBuffer cmd = AsVkCmd(m_cmd);

		const VkShaderEXT shaders[2] = {entry->vertexShader, entry->fragmentShader};
		VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
		const std::uint32_t shaderCount = (entry->fragmentShader != VK_NULL_HANDLE) ? 2u : 1u;
		vkCmdBindShadersEXT(cmd, shaderCount, stages, shaders);

		vkCmdSetPrimitiveTopologyEXT(cmd, entry->topology);
		vkCmdSetPolygonModeEXT(cmd, entry->polygonMode);
		vkCmdSetCullModeEXT(cmd, entry->cullMode);
		vkCmdSetFrontFaceEXT(cmd, entry->frontFace);
		vkCmdSetRasterizerDiscardEnableEXT(cmd, entry->rasterizerDiscardEnable);
		vkCmdSetDepthBiasEnableEXT(cmd, entry->depthBiasEnable);
		vkCmdSetPrimitiveRestartEnableEXT(cmd, entry->primitiveRestartEnable);
		vkCmdSetDepthTestEnableEXT(cmd, entry->depthTestEnable);
		vkCmdSetDepthWriteEnableEXT(cmd, entry->depthWriteEnable);
		vkCmdSetDepthCompareOpEXT(cmd, entry->depthCompareOp);
		vkCmdSetDepthBoundsTestEnableEXT(cmd, entry->depthBoundsTestEnable);
		vkCmdSetStencilTestEnableEXT(cmd, entry->stencilTestEnable);
		vkCmdSetLogicOpEXT(cmd, entry->logicOp);
		vkCmdSetBlendConstants(cmd, entry->blendConstants);
		vkCmdSetRasterizationSamplesEXT(cmd, static_cast<VkSampleCountFlagBits>(entry->rasterizationSampleCount));
		vkCmdSetSampleMaskEXT(cmd, static_cast<VkSampleCountFlagBits>(entry->rasterizationSampleCount), &entry->sampleMask);
		vkCmdSetAlphaToCoverageEnableEXT(cmd, entry->alphaToCoverageEnable);
		if (s_alphaToOneDynamicStateSupported)
		{
			vkCmdSetAlphaToOneEnableEXT(cmd, entry->alphaToOneEnable);
		}
		vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &entry->colorBlendEnable);
		vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &entry->colorBlendEquation);
		vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &entry->colorWriteMask);
		if (entry->hasLineWidth)
		{
			vkCmdSetLineWidth(cmd, entry->lineWidth);
		}
		if (!entry->vertexBindings.empty())
		{
			vkCmdSetVertexInputEXT(cmd, static_cast<std::uint32_t>(entry->vertexBindings.size()), entry->vertexBindings.data(), static_cast<std::uint32_t>(entry->vertexAttributes.size()), entry->vertexAttributes.data());
		}
		else
		{
			vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
		}
	}

	void CommandList::BindPipeline(GraphicsPipeline& pipeline)
	{
		BindPipeline(pipeline.GetPipeline());
	}

	void CommandList::Draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("Draw({}, {}, {}, {})", vertexCount, instanceCount, firstVertex, firstInstance);
		vkCmdDraw(AsVkCmd(m_cmd), vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void CommandList::DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("DrawIndexed({}, {}, {}, {}, {})", indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
		vkCmdDrawIndexed(AsVkCmd(m_cmd), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void CommandList::Dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("Dispatch({}, {}, {})", groupCountX, groupCountY, groupCountZ);
		vkCmdDispatch(AsVkCmd(m_cmd), groupCountX, groupCountY, groupCountZ);
	}

	void CommandList::BindComputePipeline(PipelineView pipeline) noexcept
	{
		if (m_cmd == nullptr || pipeline == nullptr)
		{
			return;
		}
		const auto* entry = static_cast<const ::aether::ResourceRegistry::PipelineEntry*>(pipeline);
		const VkCommandBuffer cmd = AsVkCmd(m_cmd);

		const VkShaderEXT shaders[1] = {entry->computeShader};
		const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
		vkCmdBindShadersEXT(cmd, 1, &stage, shaders);
	}

	void CommandList::BindIndexBuffer(void* vkBuffer, DeviceAddress offset, IndexType indexType) noexcept
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		vkCmdBindIndexBuffer2(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), VK_WHOLE_SIZE, ToVkIndexType(indexType));
	}

	void CommandList::BindIndexBuffer(BufferHandle buffer, DeviceAddress offset, IndexType indexType) noexcept
	{
		void* native = ResourceRegistry::ResolveBufferVkHandle(buffer);
		if (native == nullptr)
		{
			return;
		}
		BindIndexBuffer(native, offset, indexType);
	}

	void CommandList::BindVertexBuffer(void* vkBuffer, DeviceAddress offset) noexcept
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		const VkBuffer vkBuf = AsVkBuffer(vkBuffer);
		const auto vkOffset = static_cast<VkDeviceSize>(offset);
		vkCmdBindVertexBuffers(AsVkCmd(m_cmd), 0, 1, &vkBuf, &vkOffset);
	}

	void CommandList::SetLineWidth(float lineWidth) noexcept
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdSetLineWidth(AsVkCmd(m_cmd), lineWidth);
	}

	void CommandList::SetCullMode(CullMode cullMode) noexcept
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdSetCullModeEXT(AsVkCmd(m_cmd), ToVk(cullMode));
	}

	void CommandList::DrawIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("DrawIndirect(count={}, stride={})", drawCount, stride);
		vkCmdDrawIndirect(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), drawCount, stride);
	}

	void CommandList::DrawIndexedIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("DrawIndexedIndirect(count={}, stride={})", drawCount, stride);
		vkCmdDrawIndexedIndirect(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), drawCount, stride);
	}

	void CommandList::DrawIndexedIndirectCount(void* vkIndirectBuffer, DeviceAddress indirectOffset, void* vkCountBuffer, DeviceAddress countOffset, std::uint32_t maxDrawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkIndirectBuffer == nullptr || vkCountBuffer == nullptr)
		{
			return;
		}
		RecordDiagnosticEventFmt("DrawIndexedIndirectCount(max={}, stride={})", maxDrawCount, stride);
		vkCmdDrawIndexedIndirectCount(AsVkCmd(m_cmd), AsVkBuffer(vkIndirectBuffer), static_cast<VkDeviceSize>(indirectOffset), AsVkBuffer(vkCountBuffer), static_cast<VkDeviceSize>(countOffset), maxDrawCount, stride);
	}

	void CommandList::FillBuffer(void* vkBuffer, DeviceAddress offset, DeviceAddress size, std::uint32_t value) noexcept
	{
		if (m_cmd == nullptr || vkBuffer == nullptr || size == 0)
		{
			return;
		}
		RecordDiagnosticEventFmt("FillBuffer(size={}, value=0x{:X})", size, value);
		vkCmdFillBuffer(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(size), value);
	}

	void CommandList::SetViewport(const Viewport& viewport)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		const VkViewport vkVp = ToVkViewport(viewport);
		vkCmdSetViewportWithCount(AsVkCmd(m_cmd), 1, &vkVp);
	}

	void CommandList::SetViewport(std::uint32_t firstViewport, std::span<const Viewport> viewports)
	{
		if (m_cmd == nullptr || viewports.empty())
		{
			return;
		}
		const std::size_t n = viewports.size();
		VkViewport stackBuf[8];
		VkViewport* buf = stackBuf;
		std::vector<VkViewport> heapBuf;
		if (n > std::size(stackBuf))
		{
			heapBuf.resize(n);
			buf = heapBuf.data();
		}
		for (std::size_t i = 0; i < n; ++i)
		{
			buf[i] = ToVkViewport(viewports[i]);
		}
		if (firstViewport == 0)
		{
			vkCmdSetViewportWithCount(AsVkCmd(m_cmd), static_cast<std::uint32_t>(n), buf);
		}
		else
		{
			vkCmdSetViewport(AsVkCmd(m_cmd), firstViewport, static_cast<std::uint32_t>(n), buf);
		}
	}

	void CommandList::SetScissor(const Rect2D& scissor)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		const VkRect2D vkRect = ToVkRect2D(scissor);
		vkCmdSetScissorWithCount(AsVkCmd(m_cmd), 1, &vkRect);
	}

	void CommandList::SetScissor(std::uint32_t firstScissor, std::span<const Rect2D> scissors)
	{
		if (m_cmd == nullptr || scissors.empty())
		{
			return;
		}
		const std::size_t n = scissors.size();
		VkRect2D stackBuf[8];
		VkRect2D* buf = stackBuf;
		std::vector<VkRect2D> heapBuf;
		if (n > std::size(stackBuf))
		{
			heapBuf.resize(n);
			buf = heapBuf.data();
		}
		for (std::size_t i = 0; i < n; ++i)
		{
			buf[i] = ToVkRect2D(scissors[i]);
		}
		if (firstScissor == 0)
		{
			vkCmdSetScissorWithCount(AsVkCmd(m_cmd), static_cast<std::uint32_t>(n), buf);
		}
		else
		{
			vkCmdSetScissor(AsVkCmd(m_cmd), firstScissor, static_cast<std::uint32_t>(n), buf);
		}
	}

	void CommandList::PushDataRaw(std::uint32_t offset, std::span<const std::byte> data)
	{
		if (m_cmd == nullptr || data.empty())
		{
			return;
		}
		// vkCmdPushDataEXT is layout-free and belongs to VK_EXT_descriptor_heap. Without that
		// extension the same payload goes through ordinary push constants against the one
		// global pipeline layout BindlessManager published.
		if (const auto& global = vulkan::GetGlobalBindingLayout(); global.pipelineLayout != VK_NULL_HANDLE)
		{
			vkCmdPushConstants(AsVkCmd(m_cmd), global.pipelineLayout, VK_SHADER_STAGE_ALL, offset, static_cast<std::uint32_t>(data.size()), data.data());
			return;
		}

		const VkPushDataInfoEXT pushInfo{
		        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
		        .pNext = nullptr,
		        .offset = offset,
		        .data = {data.data(), data.size()},
		};
		vkCmdPushDataEXT(AsVkCmd(m_cmd), &pushInfo);
	}

	void CommandList::BeginDebugLabel(std::string_view name, float r, float g, float b, float a)
	{
		if (m_cmd == nullptr || name.empty())
		{
			return;
		}
		RecordDiagnosticEventFmt("BeginDebugLabel({})", name);
		auto fn = s_beginDebugLabel;
		if (fn == nullptr)
		{
			return;
		}
		VkDebugUtilsLabelEXT label{};
		label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
		label.pLabelName = name.data();
		label.color[0] = r;
		label.color[1] = g;
		label.color[2] = b;
		label.color[3] = a;
		fn(AsVkCmd(m_cmd), &label);
	}

	void CommandList::EndDebugLabel()
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		RecordDiagnosticEvent("EndDebugLabel");
		auto fn = s_endDebugLabel;
		if (fn == nullptr)
		{
			return;
		}
		fn(AsVkCmd(m_cmd));
	}

	void CommandList::PipelineMemoryBarrier(PipelineStage srcStage, AccessFlags srcAccess, PipelineStage dstStage, AccessFlags dstAccess) noexcept
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		// GpuEnumConversions so the engine code never sees Vk* constants.
		const VkMemoryBarrier2 barrier{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		        .pNext = nullptr,
		        .srcStageMask = ToVk(srcStage),
		        .srcAccessMask = ToVk(srcAccess),
		        .dstStageMask = ToVk(dstStage),
		        .dstAccessMask = ToVk(dstAccess),
		};
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .pNext = nullptr,
		        .dependencyFlags = 0,
		        .memoryBarrierCount = 1,
		        .pMemoryBarriers = &barrier,
		        .bufferMemoryBarrierCount = 0,
		        .pBufferMemoryBarriers = nullptr,
		        .imageMemoryBarrierCount = 0,
		        .pImageMemoryBarriers = nullptr,
		};
		vkCmdPipelineBarrier2(AsVkCmd(m_cmd), &dep);
	}

	void CommandList::ImageMemoryBarrier(void* image, ImageLayout oldLayout, ImageLayout newLayout, ImageAspect aspect, PipelineStage srcStage, AccessFlags srcAccess, PipelineStage dstStage, AccessFlags dstAccess) noexcept
	{
		if (m_cmd == nullptr || image == nullptr)
		{
			return;
		}
		const VkImageMemoryBarrier2 barrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .pNext = nullptr,
		        .srcStageMask = ToVk(srcStage),
		        .srcAccessMask = ToVk(srcAccess),
		        .dstStageMask = ToVk(dstStage),
		        .dstAccessMask = ToVk(dstAccess),
		        .oldLayout = ToVk(oldLayout),
		        .newLayout = ToVk(newLayout),
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = static_cast<VkImage>(image),
		        .subresourceRange =
		                {
		                        .aspectMask = ToVk(aspect),
		                        .baseMipLevel = 0,
		                        .levelCount = 1,
		                        .baseArrayLayer = 0,
		                        .layerCount = 1,
		                },
		};
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .pNext = nullptr,
		        .dependencyFlags = 0,
		        .memoryBarrierCount = 0,
		        .pMemoryBarriers = nullptr,
		        .bufferMemoryBarrierCount = 0,
		        .pBufferMemoryBarriers = nullptr,
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(AsVkCmd(m_cmd), &dep);
	}

	void CommandList::BeginRendering(const gpu::RenderingInfo& info)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		std::vector<VkRenderingAttachmentInfo> vkColorAttachments;
		vkColorAttachments.reserve(info.colorAttachments.size());
		for (const auto& a: info.colorAttachments)
		{
			vkColorAttachments.push_back(gpu::ToVk(a));
		}
		VkRenderingAttachmentInfo depthAttachmentVk{};
		const VkRenderingAttachmentInfo* pDepthAttachment = nullptr;
		if (info.depthAttachment != nullptr)
		{
			depthAttachmentVk = gpu::ToVk(*info.depthAttachment);
			pDepthAttachment = &depthAttachmentVk;
		}
		const VkRenderingInfo vkInfo = gpu::ToVk(info, vkColorAttachments.data(), static_cast<std::uint32_t>(vkColorAttachments.size()), pDepthAttachment);
		vkCmdBeginRendering(AsVkCmd(m_cmd), &vkInfo);
	}

	void CommandList::EndRendering()
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdEndRendering(AsVkCmd(m_cmd));
	}

	void CommandList::WriteTimestamp(void* queryPool, std::uint32_t slot, PipelineStage stage) noexcept
	{
		if (m_cmd == nullptr || queryPool == nullptr)
		{
			return;
		}
		vkCmdWriteTimestamp2(AsVkCmd(m_cmd), ToVk(stage), static_cast<VkQueryPool>(queryPool), slot);
	}

	void CommandList::ResetQueryPool(void* queryPool, std::uint32_t firstSlot, std::uint32_t slotCount) noexcept
	{
		if (m_cmd == nullptr || queryPool == nullptr || slotCount == 0u)
		{
			return;
		}
		vkCmdResetQueryPool(AsVkCmd(m_cmd), static_cast<VkQueryPool>(queryPool), firstSlot, slotCount);
	}

	void CommandList::CopyBuffer(void* src, void* dst, std::uint64_t srcOffset, std::uint64_t dstOffset, std::uint64_t size) noexcept
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		const VkBufferCopy region{
		        .srcOffset = static_cast<VkDeviceSize>(srcOffset),
		        .dstOffset = static_cast<VkDeviceSize>(dstOffset),
		        .size = static_cast<VkDeviceSize>(size),
		};
		vkCmdCopyBuffer(AsVkCmd(m_cmd), static_cast<VkBuffer>(src), static_cast<VkBuffer>(dst), 1, &region);
	}

	void CommandList::CopyImageToBuffer(
	        void* srcImage, void* dstBuffer, ImageLayout srcImageLayout, ImageAspect aspect, std::uint32_t width, std::uint32_t height, std::uint64_t bufferOffset, std::int32_t imageOffsetX, std::int32_t imageOffsetY) noexcept
	{
		if (m_cmd == nullptr || srcImage == nullptr || dstBuffer == nullptr)
		{
			return;
		}
		const VkBufferImageCopy region{
		        .bufferOffset = static_cast<VkDeviceSize>(bufferOffset),
		        .bufferRowLength = 0,
		        .bufferImageHeight = 0,
		        .imageSubresource{
		                .aspectMask = gpu::ToVk(aspect),
		                .mipLevel = 0,
		                .baseArrayLayer = 0,
		                .layerCount = 1,
		        },
		        .imageOffset{imageOffsetX, imageOffsetY, 0},
		        .imageExtent{width, height, 1},
		};
		vkCmdCopyImageToBuffer(AsVkCmd(m_cmd), static_cast<VkImage>(srcImage), ToVk(srcImageLayout), static_cast<VkBuffer>(dstBuffer), 1, &region);
	}

	void CommandList::CopyBufferToImage(
	        void* srcBuffer, void* dstImage, ImageLayout dstImageLayout, ImageAspect aspect, std::uint32_t width, std::uint32_t height, std::uint64_t bufferOffset, std::int32_t imageOffsetX, std::int32_t imageOffsetY) noexcept
	{
		if (m_cmd == nullptr || srcBuffer == nullptr || dstImage == nullptr)
		{
			return;
		}
		const VkBufferImageCopy region{
		        .bufferOffset = static_cast<VkDeviceSize>(bufferOffset),
		        .bufferRowLength = 0,
		        .bufferImageHeight = 0,
		        .imageSubresource{
		                .aspectMask = gpu::ToVk(aspect),
		                .mipLevel = 0,
		                .baseArrayLayer = 0,
		                .layerCount = 1,
		        },
		        .imageOffset{imageOffsetX, imageOffsetY, 0},
		        .imageExtent{width, height, 1},
		};
		vkCmdCopyBufferToImage(AsVkCmd(m_cmd), static_cast<VkBuffer>(srcBuffer), static_cast<VkImage>(dstImage), ToVk(dstImageLayout), 1, &region);
	}
} // namespace aether::gpu
