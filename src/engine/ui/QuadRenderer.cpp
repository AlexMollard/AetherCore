#include "ui/QuadRenderer.hpp"

#include <cstring>
#include <glm/geometric.hpp>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "rendering/CommandRecorder.hpp"
#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderGraph.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void QuadRenderer::EnsureComputePipeline()
	{
		if (m_vkCtx == nullptr || m_computePipeline != VK_NULL_HANDLE)
		{
			return;
		}

		const VkDevice device = m_vkCtx->GetDevice().device;
		AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://ui_build_draws.spv"));

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "QuadRenderer"));

		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		        .offset = 0,
		        .size = sizeof(ComputePush),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			Throw(AetherError::Vulkan(0, "QuadRenderer: failed to create compute pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo stage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = "main",
		};
		const VkComputePipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = stage,
		        .layout = m_computePipelineLayout,
		};
		if (vkCreateComputePipelines(device, m_vkCtx->GetPipelineCache(), 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			vkDestroyPipelineLayout(device, m_computePipelineLayout, nullptr);
			m_computePipelineLayout = VK_NULL_HANDLE;
			Throw(AetherError::Vulkan(0, "QuadRenderer: failed to create compute pipeline."));
		}

		vkDestroyShaderModule(device, shaderModule, nullptr);
		CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_computePipeline), VK_OBJECT_TYPE_PIPELINE, "UI.BuildDraws");
	}

	void QuadRenderer::RegisterPass()
	{
		if (m_vkCtx == nullptr)
		{
			return;
		}
		EnsureComputePipeline();

		m_buildPassName = m_passName + ".Build";
		const VkPipeline computePipeline = m_computePipeline;
		const VkPipelineLayout computeLayout = m_computePipelineLayout;
		m_renderGraph->AddComputePass(m_buildPassName)
		        .ExecuteCompute(
		                [this, computePipeline, computeLayout](PassContext& ctx)
		                {
			                if (m_vkCtx == nullptr)
			                {
				                return;
			                }

			                const std::uint32_t readSlot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                const auto& pending = m_pendingQuads[readSlot];
			                const std::uint32_t commandCount = static_cast<std::uint32_t>(pending.size());
			                if (commandCount == 0)
			                {
				                return;
			                }

			                const std::uint32_t frameSlot = readSlot;
			                const VkDeviceSize commandBytes = static_cast<VkDeviceSize>(pending.size() * sizeof(DrawCommandData));
			                if (!m_commandBuffers[frameSlot] || m_commandBufferCapacities[frameSlot] < static_cast<std::size_t>(commandBytes))
			                {
				                m_commandBuffers[frameSlot].Reset();

				                const VkDeviceSize allocSize = commandBytes * 2;
				                VkBufferCreateInfo bufferInfo{
				                        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				                        .size = allocSize,
				                        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				                };

				                VmaAllocationCreateInfo allocInfo{};
				                allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

				                AE_EXPECT_OR_THROW(buf, UniqueBuffer::Create(m_vkCtx->GetAllocator(), m_vkCtx->GetDevice().device, bufferInfo, allocInfo));
				                m_commandBuffers[frameSlot] = std::move(buf);
				                m_commandBufferCapacities[frameSlot] = static_cast<std::size_t>(allocSize);
			                }

			                if (!m_indirectBuffers[frameSlot])
			                {
				                const VkBufferCreateInfo indirectInfo{
				                        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				                        .size = sizeof(VkDrawIndirectCommand),
				                        .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				                };
				                VmaAllocationCreateInfo allocInfo{};
				                allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				                AE_EXPECT_OR_THROW(buf, UniqueBuffer::Create(m_vkCtx->GetAllocator(), m_vkCtx->GetDevice().device, indirectInfo, allocInfo));
				                m_indirectBuffers[frameSlot] = std::move(buf);
			                }

			                void* mappedCommands = m_commandBuffers[frameSlot].GetAllocationInfo().pMappedData;
			                if (mappedCommands == nullptr)
			                {
				                return;
			                }

			                std::memcpy(mappedCommands, pending.data(), commandBytes);

			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
			                cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderRead | gpu::AccessFlags::ShaderWrite);

			                const ComputePush push{
			                        .commandDataAddr = m_commandBuffers[frameSlot].GetDeviceAddress(),
			                        .indirectCmdAddr = m_indirectBuffers[frameSlot].GetDeviceAddress(),
			                        .commandCount = commandCount,
			                };

			                cmd.BindComputePipeline(static_cast<void*>(computePipeline), static_cast<void*>(computeLayout));
			                cmd.PushConstantsRaw(static_cast<gpu::PipelineLayout>(computeLayout), gpu::ShaderStage::Compute, 0, std::as_bytes(std::span{&push, 1}));
			                cmd.Dispatch(1, 1, 1);

			                // Barrier here (outside any render pass) - compute writes must be
			                // visible to the subsequent indirect-draw and vertex-shader reads.
			                cmd.PipelineMemoryBarrier(
			                        gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderWrite, gpu::PipelineStage::DrawIndirect | gpu::PipelineStage::VertexShader, gpu::AccessFlags::IndirectCommandRead | gpu::AccessFlags::ShaderRead);
		                });

		auto color = m_renderGraph->GetSwapchainColor();
		m_renderGraph->AddPass(m_passName)
		        .WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_vkCtx == nullptr)
			                {
				                return;
			                }
			                const std::uint32_t readSlot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                if (m_pendingQuads[readSlot].empty())
			                {
				                return;
			                }

			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
			                const VkExtent2D ext = ctx.extent;
			                const std::uint32_t frameSlot = readSlot;

			                const gpu::Viewport viewport{
			                        .x = 0.f,
			                        .y = 0.f,
			                        .width = static_cast<float>(ext.width),
			                        .height = static_cast<float>(ext.height),
			                        .minDepth = 0.f,
			                        .maxDepth = 1.f,
			                };
			                const gpu::Rect2D scissor{
			                        .x = 0,
			                        .y = 0,
			                        .width = ext.width,
			                        .height = ext.height,
			                };
			                cmd.SetViewport(viewport);
			                cmd.SetScissor(scissor);

			                cmd.BindPipeline(m_pipeline);

			                // Bind the global bindless descriptor set so textured
			                // rect draws can sample textures. Always bound even
			                // for non-textured shapes since the pipeline layout
			                // declares the set.
			                cmd.BindDescriptorSet(0, m_bindlessMgr->GetSet());

			                const QuadPush push{
			                        .screenSize = glm::vec4(static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 0.f),
			                        .commandDataAddr = m_commandBuffers[frameSlot].GetDeviceAddress(),
			                };
			                cmd.PushConstantsRaw(gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, std::as_bytes(std::span{&push, 1}));

			                cmd.DrawIndirect(m_indirectBuffers[frameSlot].Get(), 0, 1, sizeof(VkDrawIndirectCommand));

			                m_pendingQuads[readSlot].clear();
		                });
	}

	void QuadRenderer::ReRegisterPass()
	{
		if (!m_ready || m_vkCtx == nullptr)
		{
			return;
		}
		RegisterPass();
	}

	void QuadRenderer::Init(ServiceContainer& services, std::string_view passName)
	{
		m_vkCtx = &services.Get<VulkanContext>();
		m_renderGraph = &services.Get<RenderGraph>();
		m_bindlessMgr = &services.Get<BindlessManager>();
		m_swapchain = &services.Get<Swapchain>();
		m_passName = std::string(passName);

		for (auto& slot: m_pendingQuads)
		{
			slot.reserve(256);
		}

		const aether::gpu::DescriptorSetLayout bindlessLayout = m_bindlessMgr->GetLayout();

		AE_EXPECT_OR_THROW(pipeline,
		        services.Get<AssetManager>().CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://ui_shapes.spv",
		                .colorFormat = m_swapchain->GetImageFormat(),
		                .depthFormat = VK_FORMAT_UNDEFINED,
		                .depthTestEnable = false,
		                .depthWriteEnable = false,
		                .blendEnable = true,
		                .pushConstantSize = static_cast<uint32_t>(sizeof(QuadPush)),
		                .pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(&bindlessLayout, 1),
		        }));
		m_pipeline = std::move(pipeline);

		RegisterPass();
		m_ready = true;
		AE_INFO(LogCategory::Engine, "QuadRenderer: pass '{}' registered.", m_passName);
	}

	void QuadRenderer::Shutdown(ServiceContainer& services)
	{
		if (m_ready)
		{
			services.Get<RenderGraph>().RemovePass(m_buildPassName);
			services.Get<RenderGraph>().RemovePass(m_passName);
			m_pipeline.Destroy();
			if (m_computePipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(services.Get<VulkanContext>().GetDevice().device, m_computePipeline, nullptr);
				m_computePipeline = VK_NULL_HANDLE;
			}
			if (m_computePipelineLayout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(services.Get<VulkanContext>().GetDevice().device, m_computePipelineLayout, nullptr);
				m_computePipelineLayout = VK_NULL_HANDLE;
			}
			for (auto& slot: m_pendingQuads)
			{
				slot.clear();
			}
			for (auto& buffer: m_commandBuffers)
			{
				buffer.Reset();
			}
			for (auto& buffer: m_indirectBuffers)
			{
				buffer.Reset();
			}
			m_commandBufferCapacities.fill(0);
			m_buildPassName.clear();
			m_vkCtx = nullptr;
			m_renderGraph = nullptr;
			m_bindlessMgr = nullptr;
			m_swapchain = nullptr;
			m_ready = false;
		}
	}

	void QuadRenderer::DrawRect(const UiRect& rect, glm::vec4 color, std::int32_t layer, float cornerRadiusPx)
	{
		if (m_vkCtx == nullptr)
		{
			return;
		}

		const glm::vec4 pxRect = ResolveUiRectPx(m_swapchain->GetExtent(), rect);

		if (!m_ready || pxRect.z <= 0.0f || pxRect.w <= 0.0f || IsClipped(pxRect))
		{
			return;
		}

		m_pendingQuads[m_writeSlot].push_back({
		        .cmd =
		                DrawCommandData{
		                        .data0 = pxRect,
		                        .data1 = glm::vec4(cornerRadiusPx, 0.f, 0.f, 0.f),
		                        .color = color,
		                        .type = static_cast<std::uint32_t>(ShapeType::Rect),
		                        .layer = layer,
		                },
		});
	}

	void QuadRenderer::DrawLine(const UiPoint& start, const UiPoint& end, float thicknessPx, glm::vec4 color, std::int32_t layer)
	{
		if (m_vkCtx == nullptr || !m_ready || thicknessPx <= 0.0f)
		{
			return;
		}

		const VkExtent2D ext = m_swapchain->GetExtent();
		const glm::vec2 p0 = ResolveUiPointPx(ext, start);
		const glm::vec2 p1 = ResolveUiPointPx(ext, end);
		if (glm::length(p1 - p0) <= 0.5f)
		{
			return;
		}

		m_pendingQuads[m_writeSlot].push_back({
		        .cmd =
		                DrawCommandData{
		                        .data0 = glm::vec4(p0, p1),
		                        .data1 = glm::vec4(thicknessPx, 0.f, 0.f, 0.f),
		                        .color = color,
		                        .type = static_cast<std::uint32_t>(ShapeType::Line),
		                        .layer = layer,
		                },
		});
	}

	void QuadRenderer::DrawCircle(const UiPoint& center, float radiusPx, glm::vec4 color, std::int32_t layer)
	{
		if (m_vkCtx == nullptr || !m_ready || radiusPx <= 0.0f)
		{
			return;
		}
		const glm::vec2 c = ResolveUiPointPx(m_swapchain->GetExtent(), center);
		m_pendingQuads[m_writeSlot].push_back({
		        .cmd =
		                DrawCommandData{
		                        .data0 = glm::vec4(c.x, c.y, radiusPx, 0.f),
		                        .data1 = glm::vec4(0.f),
		                        .color = color,
		                        .type = static_cast<std::uint32_t>(ShapeType::Circle),
		                        .layer = layer,
		                },
		});
	}

	void QuadRenderer::DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect, glm::vec4 tint, std::int32_t layer)
	{
		if (m_vkCtx == nullptr || !m_ready)
		{
			return;
		}

		const glm::vec4 pxRect = ResolveUiRectPx(m_swapchain->GetExtent(), rect);
		if (pxRect.z <= 0.f || pxRect.w <= 0.f || IsClipped(pxRect))
		{
			return;
		}

		m_pendingQuads[m_writeSlot].push_back({
		        .cmd =
		                DrawCommandData{
		                        .data0 = pxRect,
		                        .data1 = uvRect, // u0, v0, u1, v1
		                        .color = tint,
		                        .type = static_cast<std::uint32_t>(ShapeType::TexturedRect),
		                        .layer = layer,
		                        .textureSlot = textureSlot,
		                },
		});
	}

	void QuadRenderer::DrawGlyph(glm::vec4 glyphRectPx, glm::vec4 uvRect, glm::vec4 color, std::uint32_t atlasSlot, std::int32_t layer)
	{
		if (!m_ready || glyphRectPx.z <= 0.f || glyphRectPx.w <= 0.f || IsClipped(glyphRectPx))
		{
			return;
		}

		m_pendingQuads[m_writeSlot].push_back({
		        .cmd =
		                DrawCommandData{
		                        .data0 = glyphRectPx,
		                        .data1 = uvRect,
		                        .color = color,
		                        .type = static_cast<std::uint32_t>(ShapeType::SdfGlyph),
		                        .layer = layer,
		                        .textureSlot = atlasSlot,
		                },
		});
	}

	void QuadRenderer::SetClipRect(glm::vec4 pixelRect)
	{
		m_clipState = {.active = true, .pixelRect = pixelRect};
	}

	void QuadRenderer::ClearClipRect()
	{
		m_clipState = {};
	}

	bool QuadRenderer::IsClipped(glm::vec4 pxRect) const
	{
		if (!m_clipState.active)
		{
			return false;
		}
		const glm::vec4& c = m_clipState.pixelRect;
		// Entirely outside if one rect is to the left/right/above/below the other.
		return (pxRect.x + pxRect.z <= c.x) || (pxRect.x >= c.x + c.z) || (pxRect.y + pxRect.w <= c.y) || (pxRect.y >= c.y + c.w);
	}
} // namespace aether
