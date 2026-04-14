#include "TextRenderer.hpp"

#include <vulkan/vulkan.h>

#include "BindlessManager.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"
#include "MeowCore.hpp"
#include "RenderGraph.hpp"
#include "VulkanContext.hpp"

namespace meow
{
	void TextRenderer::RegisterPass()
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
					if (m_engine == nullptr)
						return;

					if (m_pendingLabels.empty() || !m_fontAtlas.IsValid())
					{
						return;
					}

					MeowCore& engine = *m_engine;
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

					const VkDescriptorSet bindlessSet = engine.GetBindlessManager().GetSet();
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_pipeline.GetLayout(), 0, 1, &bindlessSet, 0, nullptr);

					const glm::vec4 screenSize{
						static_cast<float>(ext.width),
						static_cast<float>(ext.height),
						0.f, 0.f,
					};

					for (const PendingLabel& label : m_pendingLabels)
					{
						float cursorX = label.position.x;

						for (char c : label.text)
						{
							const GlyphInfo& g = m_fontAtlas.GetGlyph(c);

							if (c == ' ' || g.width == 0.f || g.height == 0.f)
							{
								cursorX += g.advanceX * label.fontSize;
								continue;
							}

							const GlyphPush push{
								.screenSize = screenSize,
								.glyphRect = glm::vec4(
									cursorX + g.bearingX * label.fontSize,
									label.position.y - g.bearingY * label.fontSize,
									g.width * label.fontSize,
									g.height * label.fontSize),
								.uvRect = g.uvRect,
								.color = label.color,
								.atlasSlot = m_fontAtlas.GetBindlessSlot(),
							};

							vkCmdPushConstants(
								cmd,
								m_pipeline.GetLayout(),
								VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
								0, sizeof(GlyphPush), &push);

							ctx.recorder.Draw(6);
							cursorX += g.advanceX * label.fontSize;
						}
					}
					m_pendingLabels.clear();
				});
	}

	void TextRenderer::EnsurePassRegistered()
	{
		if (!m_ready || m_engine == nullptr)
		{
			return;
		}

		if (!m_engine->GetRenderGraph().HasPass(m_passName))
		{
			RegisterPass();
			INFO(LogCategory::Engine,
				"TextRenderer: pass '{}' re-registered after graph reset.",
				m_passName);
		}
	}

	void TextRenderer::Init(
		MeowCore& engine,
		std::string_view fontVfsPath,
		std::string_view passName,
		int              glyphSize)
	{
		m_engine = &engine;
		m_passName = std::string(passName);

		if (!io::FileSystem::Exists(fontVfsPath))
		{
			WARN(LogCategory::Asset,
				"TextRenderer: font not found at '{}'. Text rendering disabled. "
				"Place a .ttf file at that VFS path.",
				fontVfsPath);
			return;
		}

		const VulkanContext& vk = engine.GetVulkanContext();
		m_fontAtlas.Build(
			fontVfsPath,
			glyphSize,
			vk.GetDevice().device,
			vk.GetAllocator(),
			vk.GetGraphicsQueue(),
			vk.GetGraphicsQueueFamily(),
			engine.GetBindlessManager());

		const VkDescriptorSetLayout bindlessLayout =
			engine.GetBindlessManager().GetLayout();

		m_pipeline = engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://text_sdf.slang.spv",
			// Target the swapchain image — UIPass runs after tonemapping.
			.colorFormat = engine.GetSwapchainImageFormat(),
			.depthFormat = VK_FORMAT_UNDEFINED,
			.depthTestEnable = false,
			.depthWriteEnable = false,
			.blendEnable = true,
			.noVertexInput = true,
			.pushConstantSize = static_cast<uint32_t>(sizeof(GlyphPush)),
			.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		RegisterPass();

		m_ready = true;
		INFO(LogCategory::Engine,
			"TextRenderer: pass '{}' registered, font atlas ready.", m_passName);
	}

	void TextRenderer::Shutdown(MeowCore& engine)
	{
		if (m_ready)
		{
			engine.GetRenderGraph().RemovePass(m_passName);
			m_pipeline.Destroy();
			m_fontAtlas.Destroy();
			m_pendingLabels.clear();
			m_engine = nullptr;
			m_ready = false;
		}
	}

	void TextRenderer::DrawText(
		std::string_view text,
		const UiPoint& point,
		float fontSize,
		glm::vec4 color)
	{
		EnsurePassRegistered();

		if (m_engine == nullptr)
		{
			return;
		}

		if (!m_ready || text.empty())
		{
			return;
		}

		const glm::vec2 px = ResolveUiPointPx(m_engine->GetSwapchainExtent(), point);
		m_pendingLabels.push_back({ std::string(text), px, fontSize, color });
	}

}
