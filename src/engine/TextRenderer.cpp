#include "TextRenderer.hpp"

#include <cstring>
#include <format>
#include <vk_mem_alloc.h>
#include "volk.hpp"

#include "AetherCore.hpp"
#include "BindlessManager.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"
#include "RenderGraph.hpp"
#include "VulkanContext.hpp"

namespace aether
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
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_engine == nullptr)
			                {
				                return;
			                }

			                const std::uint32_t readSlot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                if (m_pendingLabels[readSlot].empty() || !m_fontAtlas.IsValid())
			                {
				                return;
			                }

			                AetherCore& engine = *m_engine;
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

			                std::vector<GlyphInstance> glyphs;
			                glyphs.reserve(256);

			                for (const PendingLabel& label: m_pendingLabels[readSlot])
			                {
				                float cursorX = label.position.x;

				                for (char c: label.text)
				                {
					                const GlyphInfo& g = m_fontAtlas.GetGlyph(c);

					                if (c == ' ' || g.width == 0.f || g.height == 0.f)
					                {
						                cursorX += g.advanceX * label.fontSize;
						                continue;
					                }

					                glyphs.push_back({
					                        .glyphRect = glm::vec4(cursorX + g.bearingX * label.fontSize, label.position.y - g.bearingY * label.fontSize, g.width * label.fontSize, g.height * label.fontSize),
					                        .uvRect = g.uvRect,
					                        .color = label.color,
					                });

					                cursorX += g.advanceX * label.fontSize;
				                }
			                }

			                if (glyphs.empty())
			                {
				                m_pendingLabels[readSlot].clear();
				                return;
			                }

			                const std::string textLabel = std::format("Text.Batch ({} glyphs)", glyphs.size());
			                ctx.recorder.BeginDebugLabel(textLabel.c_str(), 0.85f, 0.35f, 0.70f, 1.0f);

			                const VkDeviceSize glyphBytes = static_cast<VkDeviceSize>(glyphs.size() * sizeof(GlyphInstance));

			                if (!m_glyphBuffers[frameSlot] || m_glyphBufferCapacities[frameSlot] < static_cast<std::size_t>(glyphBytes))
			                {
				                m_glyphBuffers[frameSlot].Reset();

				                VkBufferCreateInfo bufferInfo{
					                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					                .pNext = nullptr,
					                .flags = 0,
					                .size = glyphBytes,
					                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
					                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
					                .queueFamilyIndexCount = 0,
					                .pQueueFamilyIndices = nullptr,
				                };

				                VmaAllocationCreateInfo allocInfo{};
				                allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

				                m_glyphBuffers[frameSlot] = UniqueBuffer::Create(engine.GetVulkanContext().GetAllocator(), engine.GetVulkanContext().GetDevice().device, bufferInfo, allocInfo);

				                m_glyphBufferCapacities[frameSlot] = static_cast<std::size_t>(glyphBytes);
			                }

			                void* mappedPtr = m_glyphBuffers[frameSlot].GetAllocationInfo().pMappedData;
			                if (mappedPtr == nullptr)
			                {
				                m_pendingLabels[readSlot].clear();
				                return;
			                }

			                std::memcpy(mappedPtr, glyphs.data(), static_cast<std::size_t>(glyphBytes));

			                ctx.recorder.BindGraphicsPipeline(m_pipeline);

			                const VkDescriptorSet bindlessSet = engine.GetBindlessManager().GetSet();
			                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetLayout(), 0, 1, &bindlessSet, 0, nullptr);

			                const BatchPush push{
				                .screenSize = glm::vec4(static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 0.f),
				                .atlasSlot = m_fontAtlas.GetBindlessSlot(),
				                ._pad0 = 0,
				                .glyphDataAddr = m_glyphBuffers[frameSlot].GetDeviceAddress(),
			                };

			                vkCmdPushConstants(cmd, m_pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BatchPush), &push);

			                ctx.recorder.Draw(static_cast<std::uint32_t>(glyphs.size() * 6));
			                ctx.recorder.EndDebugLabel();
			                m_pendingLabels[readSlot].clear();
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
			INFO(LogCategory::Engine, "TextRenderer: pass '{}' re-registered after graph reset.", m_passName);
		}
	}

	void TextRenderer::Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passName, int glyphSize)
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
		m_fontAtlas.Build(fontVfsPath, glyphSize, vk.GetDevice().device, vk.GetAllocator(), vk.GetGraphicsQueue(), vk.GetGraphicsQueueFamily(), engine.GetBindlessManager());

		const VkDescriptorSetLayout bindlessLayout = engine.GetBindlessManager().GetLayout();

		m_pipeline = engine.CreateGraphicsPipeline({
		        .shaderVfsPath = "shaders://text_sdf.slang.spv",
		        // Target the swapchain image - UIPass runs after tonemapping.
		        .colorFormat = engine.GetSwapchainImageFormat(),
		        .depthFormat = VK_FORMAT_UNDEFINED,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .noVertexInput = true,
		        .pushConstantSize = static_cast<uint32_t>(sizeof(BatchPush)),
		        .pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		        .setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
		});

		RegisterPass();

		m_ready = true;
		INFO(LogCategory::Engine, "TextRenderer: pass '{}' registered, font atlas ready.", m_passName);
	}

	void TextRenderer::Shutdown(AetherCore& engine)
	{
		if (m_ready)
		{
			engine.GetRenderGraph().RemovePass(m_passName);
			m_pipeline.Destroy();
			m_fontAtlas.Destroy();
			for (UniqueBuffer& buffer: m_glyphBuffers)
			{
				buffer.Reset();
			}
			m_glyphBufferCapacities.fill(0);
			for (auto& slot: m_pendingLabels)
			{
				slot.clear();
			}
			m_engine = nullptr;
			m_ready = false;
		}
	}

	void TextRenderer::DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color)
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
		m_pendingLabels[m_writeSlot].push_back({ std::string(text), px, fontSize, color });
	}

} // namespace aether
