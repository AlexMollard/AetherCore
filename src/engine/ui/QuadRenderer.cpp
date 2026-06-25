#include "ui/QuadRenderer.hpp"

#include <algorithm>
#include <cstring>
#include <glm/geometric.hpp>
#include <vector>

#include "vulkan/VulkanUtils.hpp"
#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderGraph.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void QuadRenderer::EnsureCommandBufferReady(std::uint32_t frameSlot, gpu::DeviceSize commandBytes)
	{
		if (m_commandBuffers[frameSlot].handle.IsValid() && m_commandBuffers[frameSlot].capacity >= static_cast<std::size_t>(commandBytes))
		{
			return;
		}

		if (m_commandBuffers[frameSlot].handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_commandBuffers[frameSlot].handle);
		}

		const gpu::DeviceSize allocSize = commandBytes * 2;
		const gpu::MappedBufferDesc desc{
		        .size = allocSize,
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "QuadRenderer.Commands",
		};
		m_commandBuffers[frameSlot].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_commandBuffers[frameSlot].handle.IsValid())
		{
			Throw(AetherError::Engine("QuadRenderer: Commands CreateMappedBuffer failed"));
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_commandBuffers[frameSlot].handle);
		m_commandBuffers[frameSlot].mapped = view.mappedPtr;
		m_commandBuffers[frameSlot].address = view.deviceAddress;
		m_commandBuffers[frameSlot].capacity = static_cast<std::size_t>(view.size);
	}

	void QuadRenderer::EnsureIndirectBufferReady(std::uint32_t frameSlot)
	{
		if (m_indirectBuffers[frameSlot].handle.IsValid())
		{
			return;
		}

		const gpu::MappedBufferDesc desc{
		        .size = sizeof(gpu::DrawIndirectCommand),
		        .usage = gpu::BufferUsage::Indirect | gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "QuadRenderer.Indirect",
		};
		m_indirectBuffers[frameSlot].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_indirectBuffers[frameSlot].handle.IsValid())
		{
			Throw(AetherError::Engine("QuadRenderer: Indirect CreateMappedBuffer failed"));
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_indirectBuffers[frameSlot].handle);
		m_indirectBuffers[frameSlot].address = view.deviceAddress;
	}

	void QuadRenderer::RegisterPass()
	{
		if (m_vkCtx == nullptr)
		{
			return;
		}

		auto color = m_renderGraph->GetSwapchainColor();
		m_renderGraph->AddPass(m_passName)
		        .WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                const std::uint32_t readSlot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                if (m_vkCtx == nullptr || m_pendingQuads[readSlot].empty())
			                {
				                return;
			                }

			                const std::uint32_t frameSlot = readSlot;
			                auto& pending = m_pendingQuads[readSlot];
			                const auto commandCount = static_cast<std::uint32_t>(pending.size());
			                const auto commandBytes = static_cast<gpu::DeviceSize>(commandCount * sizeof(DrawCommandData));

			                EnsureCommandBufferReady(frameSlot, commandBytes);
			                EnsureIndirectBufferReady(frameSlot);

			                // Sort by layer on CPU.
			                std::ranges::stable_sort(pending, [](const PendingQuad& a, const PendingQuad& b) { return a.cmd.layer < b.cmd.layer; });

			                // Upload sorted command data.
			                void* mappedCommands = m_commandBuffers[frameSlot].mapped;
			                if (mappedCommands == nullptr)
			                {
				                return;
			                }
			                std::memcpy(mappedCommands, pending.data(), commandBytes);
			                gpu::ResourceRegistry::FlushMappedBuffer(m_commandBuffers[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));

			                // Write DrawIndirectCommand directly to host-visible buffer.
			                gpu::DrawIndirectCommand* indirect = static_cast<gpu::DrawIndirectCommand*>(gpu::ResourceRegistry::ResolveMappedBuffer(m_indirectBuffers[frameSlot].handle).mappedPtr);
			                if (indirect != nullptr)
			                {
				                indirect->vertexCount = 6;
				                indirect->instanceCount = commandCount;
				                indirect->firstVertex = 0;
				                indirect->firstInstance = 0;
				                gpu::ResourceRegistry::FlushMappedBuffer(m_indirectBuffers[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
			                }

			                gpu::CommandList cmd = ctx.recorder.View();
			                const gpu::Extent2D ext = ctx.extent;

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
			                m_bindlessMgr->CmdBindHeaps(cmd);

			                const QuadPush push{
			                        .screenSize = glm::vec4(static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 0.f),
			                        .commandDataAddr = m_commandBuffers[frameSlot].address,
			                };
			                cmd.PushDataRaw(0, std::as_bytes(std::span{&push, 1}));

			                cmd.DrawIndirect(gpu::ResourceRegistry::ResolveBufferVkHandle(m_indirectBuffers[frameSlot].handle), 0, 1, sizeof(gpu::DrawIndirectCommand));

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
		AE_PROFILE_ZONE();
		m_vkCtx = &services.Get<VulkanContext>();
		m_renderGraph = &services.Get<RenderGraph>();
		m_bindlessMgr = &services.Get<BindlessManager>();
		m_swapchain = &services.Get<Swapchain>();
		m_passName = std::string(passName);

		for (auto& slot: m_pendingQuads)
		{
			slot.reserve(256);
		}

		AE_EXPECT_OR_THROW(pipeline,
		        services.Get<AssetManager>().CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://ui_shapes.spv",
		                .colorFormat = m_swapchain->GetImageFormat(),
		                .depthFormat = gpu::Format::Undefined,
		                .depthTestEnable = false,
		                .depthWriteEnable = false,
		                .blendEnable = true,
		                .descriptorHeapMappings = m_bindlessMgr->GetDescriptorHeapMappings(),
		        }));
		m_pipeline = std::move(pipeline);

		RegisterPass();
		m_ready = true;
		AE_INFO(LogCategory::Engine, "QuadRenderer: pass '{}' registered.", m_passName);
	}

	void QuadRenderer::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		if (m_ready)
		{
			services.Get<RenderGraph>().RemovePass(m_passName);
			m_pipeline.Destroy();
			for (auto& slot: m_pendingQuads)
			{
				slot.clear();
			}
			for (auto& frame: m_commandBuffers)
			{
				if (frame.handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(frame.handle);
				}
				frame = {};
			}
			for (auto& frame: m_indirectBuffers)
			{
				if (frame.handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(frame.handle);
				}
				frame = {};
			}
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

		const gpu::Extent2D ext = m_swapchain->GetExtent();
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
