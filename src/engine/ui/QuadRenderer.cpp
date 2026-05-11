#include "ui/QuadRenderer.hpp"

#include <glm/geometric.hpp>
#include <stdexcept>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "scene/AetherCore.hpp"
#include "rendering/CommandRecorder.hpp"
#include "FileSystem.hpp"
#include "utils/Logger.hpp"
#include "rendering/RenderGraph.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	namespace
	{
		VkShaderModule CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv)
		{
			VkShaderModuleCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			info.codeSize = spirv.size();
			info.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
			VkShaderModule mod = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
			{
				throw std::runtime_error("QuadRenderer: failed to create compute shader module.");
			}
			return mod;
		}
	} // namespace

	void QuadRenderer::EnsureComputePipeline()
	{
		if (m_engine == nullptr || m_computePipeline != VK_NULL_HANDLE)
		{
			return;
		}

		const VkDevice device = m_engine->GetVulkanContext().GetDevice().device;
		const auto spirv = io::FileSystem::ReadFile("shaders://ui_build_draws.slang.spv");
		if (spirv.empty())
		{
			throw std::runtime_error("QuadRenderer: shader not found: shaders://ui_build_draws.slang.spv");
		}

		VkShaderModule module = CreateShaderModule(device, spirv);

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
			vkDestroyShaderModule(device, module, nullptr);
			throw std::runtime_error("QuadRenderer: failed to create compute pipeline layout.");
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		};
		const VkComputePipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage,
			.layout = m_computePipelineLayout,
		};
		if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, module, nullptr);
			vkDestroyPipelineLayout(device, m_computePipelineLayout, nullptr);
			m_computePipelineLayout = VK_NULL_HANDLE;
			throw std::runtime_error("QuadRenderer: failed to create compute pipeline.");
		}

		vkDestroyShaderModule(device, module, nullptr);
		CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_computePipeline), VK_OBJECT_TYPE_PIPELINE, "UI.BuildDraws");
	}

	void QuadRenderer::RegisterPass()
	{
		if (m_engine == nullptr)
		{
			return;
		}
		EnsureComputePipeline();

		m_buildPassName = m_passName + ".Build";
		const VkPipeline computePipeline = m_computePipeline;
		const VkPipelineLayout computeLayout = m_computePipelineLayout;
		m_engine->GetRenderGraph()
		        .AddComputePass(m_buildPassName)
		        .ExecuteCompute(
		                [this, computePipeline, computeLayout](PassContext& ctx)
		                {
			                if (m_engine == nullptr)
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

				                VkBufferCreateInfo bufferInfo{
					                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					                .size = commandBytes,
					                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
					                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				                };

				                VmaAllocationCreateInfo allocInfo{};
				                allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

				                m_commandBuffers[frameSlot] = UniqueBuffer::Create(m_engine->GetVulkanContext().GetAllocator(), m_engine->GetVulkanContext().GetDevice().device, bufferInfo, allocInfo);
				                m_commandBufferCapacities[frameSlot] = static_cast<std::size_t>(commandBytes);
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
				                m_indirectBuffers[frameSlot] = UniqueBuffer::Create(m_engine->GetVulkanContext().GetAllocator(), m_engine->GetVulkanContext().GetDevice().device, indirectInfo, allocInfo);
			                }

			                void* mappedCommands = m_commandBuffers[frameSlot].GetAllocationInfo().pMappedData;
			                if (mappedCommands == nullptr)
			                {
				                return;
			                }

			                auto* cmdData = static_cast<DrawCommandData*>(mappedCommands);
			                for (std::size_t i = 0; i < pending.size(); ++i)
			                {
				                cmdData[i] = pending[i].cmd;
			                }

			                const VkMemoryBarrier2 hostToCompute{
				                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				                .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
				                .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
				                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
			                };
			                const VkDependencyInfo hostToComputeDep{
				                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				                .memoryBarrierCount = 1,
				                .pMemoryBarriers = &hostToCompute,
			                };
			                vkCmdPipelineBarrier2(ctx.recorder.GetCommandBuffer(), &hostToComputeDep);

			                const ComputePush push{
				                .commandDataAddr = m_commandBuffers[frameSlot].GetDeviceAddress(),
				                .indirectCmdAddr = m_indirectBuffers[frameSlot].GetDeviceAddress(),
				                .commandCount = commandCount,
			                };

			                vkCmdBindPipeline(ctx.recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
			                vkCmdPushConstants(ctx.recorder.GetCommandBuffer(), computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
			                vkCmdDispatch(ctx.recorder.GetCommandBuffer(), 1, 1, 1);

			                // Barrier here (outside any render pass) - compute writes must be
			                // visible to the subsequent indirect-draw and vertex-shader reads.
			                const VkMemoryBarrier2 computeToGraphics{
				                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				                .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
				                .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
				                .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
			                };
			                const VkDependencyInfo computeToGraphicsDep{
				                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				                .memoryBarrierCount = 1,
				                .pMemoryBarriers = &computeToGraphics,
			                };
			                vkCmdPipelineBarrier2(ctx.recorder.GetCommandBuffer(), &computeToGraphicsDep);
		                });

		auto color = m_engine->GetRenderGraph().GetSwapchainColor();
		m_engine->GetRenderGraph()
		        .AddPass(m_passName)
		        .WriteColor(color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_engine == nullptr)
			                {
				                return;
			                }
			                const std::uint32_t readSlot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                if (m_pendingQuads[readSlot].empty())
			                {
				                return;
			                }

			                const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();
			                const VkExtent2D ext = ctx.extent;
			                const std::uint32_t frameSlot = readSlot;

			                const VkViewport viewport{
				                .x = 0.f,
				                .y = 0.f,
				                .width = static_cast<float>(ext.width),
				                .height = static_cast<float>(ext.height),
				                .minDepth = 0.f,
				                .maxDepth = 1.f,
			                };
			                const VkRect2D scissor{
				                .offset = { 0, 0 },
                                  .extent = ext
			                };
			                vkCmdSetViewport(cmd, 0, 1, &viewport);
			                vkCmdSetScissor(cmd, 0, 1, &scissor);

			                ctx.recorder.BindGraphicsPipeline(m_pipeline);

			                // Bind the global bindless descriptor set so textured
			                // rect draws can sample textures. Always bound even
			                // for non-textured shapes since the pipeline layout
			                // declares the set.
			                const VkDescriptorSet bindlessSet = m_engine->GetBindlessManager().GetSet();
			                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetLayout(), 0, 1, &bindlessSet, 0, nullptr);

			                const QuadPush push{
				                .screenSize = glm::vec4(static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 0.f),
				                .commandDataAddr = m_commandBuffers[frameSlot].GetDeviceAddress(),
			                };
			                vkCmdPushConstants(cmd, m_pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(QuadPush), &push);

			                ctx.recorder.DrawIndirect(m_indirectBuffers[frameSlot].Get(), 0, 1);

			                m_pendingQuads[readSlot].clear();
		                });
	}

	void QuadRenderer::EnsurePassRegistered()
	{
		if (!m_ready || m_engine == nullptr)
		{
			return;
		}

		if (!m_engine->GetRenderGraph().HasPass(m_passName))
		{
			RegisterPass();
			INFO(LogCategory::Engine, "QuadRenderer: pass '{}' re-registered after graph reset.", m_passName);
		}
	}

	void QuadRenderer::Init(AetherCore& engine, std::string_view passName)
	{
		m_engine = &engine;
		m_passName = std::string(passName);

		// Include the global bindless layout so textured rects can sample textures.
		const VkDescriptorSetLayout bindlessLayout = engine.GetBindlessManager().GetLayout();

		m_pipeline = engine.CreateGraphicsPipeline({
		        .shaderVfsPath = "shaders://ui_shapes.slang.spv",
		        .colorFormat = engine.GetSwapchainImageFormat(),
		        .depthFormat = VK_FORMAT_UNDEFINED,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .pushConstantSize = static_cast<uint32_t>(sizeof(QuadPush)),
		        .pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		        .setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
		});

		RegisterPass();
		m_ready = true;
		INFO(LogCategory::Engine, "QuadRenderer: pass '{}' registered.", m_passName);
	}

	void QuadRenderer::Shutdown(AetherCore& engine)
	{
		if (m_ready)
		{
			engine.GetRenderGraph().RemovePass(m_buildPassName);
			engine.GetRenderGraph().RemovePass(m_passName);
			m_pipeline.Destroy();
			if (m_computePipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(engine.GetVulkanContext().GetDevice().device, m_computePipeline, nullptr);
				m_computePipeline = VK_NULL_HANDLE;
			}
			if (m_computePipelineLayout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(engine.GetVulkanContext().GetDevice().device, m_computePipelineLayout, nullptr);
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
			m_engine = nullptr;
			m_ready = false;
		}
	}

	void QuadRenderer::DrawRect(const UiRect& rect, glm::vec4 color, std::int32_t layer, float cornerRadiusPx)
	{
		EnsurePassRegistered();

		if (m_engine == nullptr)
		{
			return;
		}

		const glm::vec4 pxRect = ResolveUiRectPx(m_engine->GetSwapchainExtent(), rect);

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
		EnsurePassRegistered();
		if (m_engine == nullptr || !m_ready || thicknessPx <= 0.0f)
		{
			return;
		}

		const VkExtent2D ext = m_engine->GetSwapchainExtent();
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
		EnsurePassRegistered();
		if (m_engine == nullptr || !m_ready || radiusPx <= 0.0f)
		{
			return;
		}
		const glm::vec2 c = ResolveUiPointPx(m_engine->GetSwapchainExtent(), center);
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

	void QuadRenderer::DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect, glm::vec4 tint, std::int32_t layer, float cornerRadiusPx)
	{
		EnsurePassRegistered();
		if (m_engine == nullptr || !m_ready)
		{
			return;
		}

		const glm::vec4 pxRect = ResolveUiRectPx(m_engine->GetSwapchainExtent(), rect);
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

	void QuadRenderer::SetClipRect(glm::vec4 pixelRect)
	{
		m_clipState = { .active = true, .pixelRect = pixelRect };
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
