#include "QuadRenderer.hpp"

#include <vulkan/vulkan.h>

#include "Logger.hpp"
#include "MeowCore.hpp"
#include "RenderGraph.hpp"

namespace meow
{
	void QuadRenderer::RegisterPass()
	{
		if (m_engine == nullptr)
		{
			return;
		}

		auto color = m_engine->GetRenderGraph().GetSwapchainColor();
		m_engine->GetRenderGraph()
			.AddPass(m_passName)
			.WriteColor(color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
			.Execute([this](PassContext& ctx)
				{
					if (m_engine == nullptr || m_pendingQuads.empty())
					{
						return;
					}

					const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();
					const VkExtent2D      ext = ctx.extent;

					const VkViewport viewport{
						.x = 0.f,
						.y = 0.f,
						.width = static_cast<float>(ext.width),
						.height = static_cast<float>(ext.height),
						.minDepth = 0.f,
						.maxDepth = 1.f,
					};
					const VkRect2D scissor{ .offset = {0, 0}, .extent = ext };
					vkCmdSetViewport(cmd, 0, 1, &viewport);
					vkCmdSetScissor(cmd, 0, 1, &scissor);

					ctx.recorder.BindGraphicsPipeline(m_pipeline);

					const glm::vec4 screenSize{
						static_cast<float>(ext.width),
						static_cast<float>(ext.height),
						0.f, 0.f,
					};

					for (const PendingQuad& quad : m_pendingQuads)
					{
						const QuadPush push{
							.screenSize = screenSize,
							.rect = quad.rect,
							.color = quad.color,
						};

						vkCmdPushConstants(
							cmd,
							m_pipeline.GetLayout(),
							VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
							0, sizeof(QuadPush), &push);

						ctx.recorder.Draw(6);
					}

					m_pendingQuads.clear();
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
			INFO(LogCategory::Engine,
				"QuadRenderer: pass '{}' re-registered after graph reset.",
				m_passName);
		}
	}

	void QuadRenderer::Init(MeowCore& engine, std::string_view passName)
	{
		m_engine = &engine;
		m_passName = std::string(passName);

		m_pipeline = engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://ui_quad.slang.spv",
			.colorFormat = engine.GetSwapchainImageFormat(),
			.depthFormat = VK_FORMAT_UNDEFINED,
			.depthTestEnable = false,
			.depthWriteEnable = false,
			.blendEnable = true,
			.noVertexInput = true,
			.pushConstantSize = static_cast<uint32_t>(sizeof(QuadPush)),
			.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			});

		RegisterPass();
		m_ready = true;
		INFO(LogCategory::Engine, "QuadRenderer: pass '{}' registered.", m_passName);
	}

	void QuadRenderer::Shutdown(MeowCore& engine)
	{
		if (m_ready)
		{
			engine.GetRenderGraph().RemovePass(m_passName);
			m_pipeline.Destroy();
			m_pendingQuads.clear();
			m_engine = nullptr;
			m_ready = false;
		}
	}

	void QuadRenderer::DrawQuad(const UiRect& rect, glm::vec4 color)
	{
		EnsurePassRegistered();

		if (m_engine == nullptr)
		{
			return;
		}

		const glm::vec4 pxRect = ResolveUiRectPx(m_engine->GetSwapchainExtent(), rect);

		if (!m_ready || pxRect.z <= 0.0f || pxRect.w <= 0.0f)
		{
			return;
		}

		m_pendingQuads.push_back({
			.rect = pxRect,
			.color = color,
			});
	}
}
