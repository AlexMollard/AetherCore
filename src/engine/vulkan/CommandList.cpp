#include "gpu/CommandList.hpp"

#include <vector>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/volk.hpp"

namespace aether::gpu
{
	namespace
	{
		inline VkCommandBuffer AsVkCmd(void* p) noexcept
		{
			return static_cast<VkCommandBuffer>(p);
		}

		inline VkPipeline AsVkPipeline(void* p) noexcept
		{
			return static_cast<VkPipeline>(p);
		}

		inline VkPipelineLayout AsVkPipelineLayout(void* p) noexcept
		{
			return static_cast<VkPipelineLayout>(p);
		}

		inline VkBuffer AsVkBuffer(void* p) noexcept
		{
			return static_cast<VkBuffer>(p);
		}

		inline VkImageView AsVkImageView(void* p) noexcept
		{
			return static_cast<VkImageView>(p);
		}

		inline VkSampler AsVkSampler(void* p) noexcept
		{
			return static_cast<VkSampler>(p);
		}

		inline VkDescriptorSet AsVkDescriptorSet(void* p) noexcept
		{
			return static_cast<VkDescriptorSet>(p);
		}

		inline VkShaderStageFlags ToVkShaderStages(ShaderStage s) noexcept
		{
			std::uint32_t out = 0;
			if ((static_cast<std::uint32_t>(s) & static_cast<std::uint32_t>(ShaderStage::Vertex)) != 0)
			{
				out |= VK_SHADER_STAGE_VERTEX_BIT;
			}
			if ((static_cast<std::uint32_t>(s) & static_cast<std::uint32_t>(ShaderStage::Fragment)) != 0)
			{
				out |= VK_SHADER_STAGE_FRAGMENT_BIT;
			}
			if ((static_cast<std::uint32_t>(s) & static_cast<std::uint32_t>(ShaderStage::Compute)) != 0)
			{
				out |= VK_SHADER_STAGE_COMPUTE_BIT;
			}
			return static_cast<VkShaderStageFlags>(out);
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
			        .offset = {r.x, r.y},
			        .extent = {r.width, r.height},
			};
		}

		inline VkIndexType ToVkIndexType(IndexType t) noexcept
		{
			return (t == IndexType::U16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		}

		// PFN_vkCmd*DebugUtilsLabelEXT are loaded by volk on demand. They
		// are stored statically so the labels keep working when
		// validation layers are enabled. Initialized lazily on first use;
		// null is a no-op (debug extensions not enabled in this build).
		inline PFN_vkCmdBeginDebugUtilsLabelEXT& BeginDebugLabelFn() noexcept
		{
			static PFN_vkCmdBeginDebugUtilsLabelEXT fn = nullptr;
			return fn;
		}

		inline PFN_vkCmdEndDebugUtilsLabelEXT& EndDebugLabelFn() noexcept
		{
			static PFN_vkCmdEndDebugUtilsLabelEXT fn = nullptr;
			return fn;
		}
	} // namespace

	// Static setter used by GraphicsDevice to wire the debug-label function
	// pointers at engine init.
	void CommandList::SetDebugLabelFunctions(void* beginFn, void* endFn) noexcept
	{
		BeginDebugLabelFn() = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(beginFn);
		EndDebugLabelFn() = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(endFn);
	}

	void CommandList::BindPipeline(void* vkPipeline, void* vkPipelineLayout) noexcept
	{
		if (m_cmd == nullptr || vkPipeline == nullptr)
		{
			return;
		}
		vkCmdBindPipeline(AsVkCmd(m_cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, AsVkPipeline(vkPipeline));
		m_boundLayout = vkPipelineLayout;
		m_boundBindPoint = PipelineBindPoint::Graphics;
	}

	void CommandList::BindPipeline(PipelineHandle)
	{
		// Stub: handle-based pipeline binding arrives in Phase 5 alongside
		// the GraphicsPipeline migration. For now passes use the
		// raw-void* BindPipeline overload above.
	}

	void CommandList::BindPipeline(GraphicsPipeline& pipeline)
	{
		BindPipeline(pipeline.GetPipeline(), pipeline.GetLayout());
	}

	void CommandList::Draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdDraw(AsVkCmd(m_cmd), vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void CommandList::DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdDrawIndexed(AsVkCmd(m_cmd), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void CommandList::Dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		vkCmdDispatch(AsVkCmd(m_cmd), groupCountX, groupCountY, groupCountZ);
	}

	void CommandList::BindComputePipeline(void* vkPipeline, void* vkPipelineLayout) noexcept
	{
		if (m_cmd == nullptr || vkPipeline == nullptr)
		{
			return;
		}
		vkCmdBindPipeline(AsVkCmd(m_cmd), VK_PIPELINE_BIND_POINT_COMPUTE, AsVkPipeline(vkPipeline));
		m_boundLayout = vkPipelineLayout;
		m_boundBindPoint = PipelineBindPoint::Compute;
	}

	void CommandList::BindDescriptorSet(void* vkPipelineLayout, std::uint32_t set, void* vkDescriptorSet, std::uint32_t dynamicOffsetCount, const std::uint32_t* dynamicOffsets) noexcept
	{
		if (m_cmd == nullptr || vkPipelineLayout == nullptr || vkDescriptorSet == nullptr)
		{
			return;
		}
		const VkDescriptorSet vkSet = AsVkDescriptorSet(vkDescriptorSet);
		vkCmdBindDescriptorSets(AsVkCmd(m_cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, AsVkPipelineLayout(vkPipelineLayout), set, 1, &vkSet, dynamicOffsetCount, dynamicOffsets);
	}

	void CommandList::BindDescriptorSet(std::uint32_t set, void* vkDescriptorSet, std::uint32_t dynamicOffsetCount, const std::uint32_t* dynamicOffsets) noexcept
	{
		if (m_boundLayout == nullptr)
		{
			return;
		}
		BindDescriptorSet(m_boundLayout, set, vkDescriptorSet, dynamicOffsetCount, dynamicOffsets);
	}

	void CommandList::BindIndexBuffer(void* vkBuffer, DeviceAddress offset, IndexType indexType) noexcept
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		vkCmdBindIndexBuffer(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), ToVkIndexType(indexType));
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
		const VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);
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

	void CommandList::DrawIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		vkCmdDrawIndirect(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), drawCount, stride);
	}

	void CommandList::DrawIndexedIndirect(void* vkBuffer, DeviceAddress offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkBuffer == nullptr)
		{
			return;
		}
		vkCmdDrawIndexedIndirect(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), drawCount, stride);
	}

	void CommandList::DrawIndexedIndirectCount(void* vkIndirectBuffer, DeviceAddress indirectOffset, void* vkCountBuffer, DeviceAddress countOffset, std::uint32_t maxDrawCount, std::uint32_t stride)
	{
		if (m_cmd == nullptr || vkIndirectBuffer == nullptr || vkCountBuffer == nullptr)
		{
			return;
		}
		vkCmdDrawIndexedIndirectCount(AsVkCmd(m_cmd), AsVkBuffer(vkIndirectBuffer), static_cast<VkDeviceSize>(indirectOffset), AsVkBuffer(vkCountBuffer), static_cast<VkDeviceSize>(countOffset), maxDrawCount, stride);
	}

	void CommandList::PushDescriptorSet(void* vkPipelineLayout, std::uint32_t set, std::span<const GpuWriteDescriptorSet> writes) noexcept
	{
		PushDescriptorSet(m_boundBindPoint, vkPipelineLayout, set, writes);
	}

	void CommandList::PushDescriptorSet(PipelineBindPoint bindPoint, void* vkPipelineLayout, std::uint32_t set, std::span<const GpuWriteDescriptorSet> writes) noexcept
	{
		if (m_cmd == nullptr || vkPipelineLayout == nullptr || writes.empty())
		{
			return;
		}
		// Translate engine GpuWriteDescriptorSet -> VkWriteDescriptorSet.
		// The pBufferInfo / pImageInfo / pTexelBufferView fields of the
		// VkWriteDescriptorSet must point at caller-side memory; we keep
		// small arrays alive on the stack (heap-fallback for large n).
		const std::size_t n = writes.size();
		VkDescriptorBufferInfo stackBufInfos[16];
		std::vector<VkDescriptorBufferInfo> heapBufInfos;
		VkDescriptorBufferInfo* bufInfos = stackBufInfos;
		if (n > std::size(stackBufInfos))
		{
			heapBufInfos.resize(n);
			bufInfos = heapBufInfos.data();
		}
		VkDescriptorImageInfo stackImgInfos[16];
		std::vector<VkDescriptorImageInfo> heapImgInfos;
		VkDescriptorImageInfo* imgInfos = stackImgInfos;
		if (n > std::size(stackImgInfos))
		{
			heapImgInfos.resize(n);
			imgInfos = heapImgInfos.data();
		}
		VkWriteDescriptorSet stackWrites[16];
		std::vector<VkWriteDescriptorSet> heapWrites;
		VkWriteDescriptorSet* vkWrites = stackWrites;
		if (n > std::size(stackWrites))
		{
			heapWrites.resize(n);
			vkWrites = heapWrites.data();
		}
		for (std::size_t i = 0; i < n; ++i)
		{
			const GpuWriteDescriptorSet& src = writes[i];
			VkWriteDescriptorSet& dst = vkWrites[i];
			dst = {};
			dst.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			dst.dstBinding = src.dstBinding;
			dst.descriptorCount = src.descriptorCount;
			dst.descriptorType = ToVk(src.descriptorType);
			if (src.bufferInfo != nullptr)
			{
				bufInfos[i].buffer = AsVkBuffer(src.bufferInfo->buffer);
				bufInfos[i].offset = static_cast<VkDeviceSize>(src.bufferInfo->offset);
				bufInfos[i].range = static_cast<VkDeviceSize>(src.bufferInfo->range);
				dst.pBufferInfo = &bufInfos[i];
			}
			else if (src.imageInfo != nullptr)
			{
				imgInfos[i].sampler = AsVkSampler(src.imageInfo->sampler);
				imgInfos[i].imageView = AsVkImageView(src.imageInfo->imageView);
				imgInfos[i].imageLayout = ToVk(src.imageInfo->imageLayout);
				dst.pImageInfo = &imgInfos[i];
			}
		}
		vkCmdPushDescriptorSetKHR(AsVkCmd(m_cmd), ToVk(bindPoint), AsVkPipelineLayout(vkPipelineLayout), set, static_cast<std::uint32_t>(n), vkWrites);
	}

	void CommandList::FillBuffer(void* vkBuffer, DeviceAddress offset, DeviceAddress size, std::uint32_t value) noexcept
	{
		if (m_cmd == nullptr || vkBuffer == nullptr || size == 0)
		{
			return;
		}
		vkCmdFillBuffer(AsVkCmd(m_cmd), AsVkBuffer(vkBuffer), static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(size), value);
	}

	void CommandList::SetViewport(const Viewport& viewport)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		VkViewport vkVp = ToVkViewport(viewport);
		vkCmdSetViewport(AsVkCmd(m_cmd), 0, 1, &vkVp);
	}

	void CommandList::SetViewport(std::uint32_t firstViewport, std::span<const Viewport> viewports)
	{
		if (m_cmd == nullptr || viewports.empty())
		{
			return;
		}
		// Stage a contiguous array of VkViewport (trivial copy of the same
		// fields) on the stack. The max is small enough (engine uses 1-2)
		// that a heap allocation would be wasteful.
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
		vkCmdSetViewport(AsVkCmd(m_cmd), firstViewport, static_cast<std::uint32_t>(n), buf);
	}

	void CommandList::SetScissor(const Rect2D& scissor)
	{
		if (m_cmd == nullptr)
		{
			return;
		}
		VkRect2D vkRect = ToVkRect2D(scissor);
		vkCmdSetScissor(AsVkCmd(m_cmd), 0, 1, &vkRect);
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
		vkCmdSetScissor(AsVkCmd(m_cmd), firstScissor, static_cast<std::uint32_t>(n), buf);
	}

	void CommandList::PushConstantsRaw(void* vkPipelineLayout, ShaderStage stages, std::uint32_t offset, std::span<const std::byte> data)
	{
		if (m_cmd == nullptr || vkPipelineLayout == nullptr || data.empty())
		{
			return;
		}
		vkCmdPushConstants(AsVkCmd(m_cmd), AsVkPipelineLayout(vkPipelineLayout), ToVkShaderStages(stages), offset, static_cast<std::uint32_t>(data.size()), data.data());
	}

	void CommandList::PushConstantsRaw(ShaderStage stages, std::uint32_t offset, std::span<const std::byte> data)
	{
		if (m_boundLayout == nullptr)
		{
			return;
		}
		PushConstantsRaw(m_boundLayout, stages, offset, data);
	}

	void CommandList::BeginDebugLabel(std::string_view name, float r, float g, float b, float a)
	{
		if (m_cmd == nullptr || name.empty())
		{
			return;
		}
		PFN_vkCmdBeginDebugUtilsLabelEXT fn = BeginDebugLabelFn();
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
		PFN_vkCmdEndDebugUtilsLabelEXT fn = EndDebugLabelFn();
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
		// Engine-side stage / access bit flags map 1:1 to the underlying
		// VkPipelineStageFlags2 / VkAccessFlags2 bitmask values, so the
		// translation is a straight cast. ToVk(...) funnels through
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
		                        .aspectMask = static_cast<VkImageAspectFlags>(ToVk(aspect)),
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
		// P5(d) barrier solver migration: translate the engine-side
		// gpu::RenderingInfo + its color attachment span to Vk* at the
		// seam, then call vkCmdBeginRendering. The color attachment span
		// is translated one-for-one via gpu::ToVk; the depth attachment
		// is translated on demand (it's a single attachment, not a span).
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
} // namespace aether::gpu
