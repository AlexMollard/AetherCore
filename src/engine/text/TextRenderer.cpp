#include "text/TextRenderer.hpp"

#include "utils/ServiceContainer.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/Swapchain.hpp"
#include "gpu/BindlessManager.hpp"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"
#include "ui/QuadRenderer.hpp"

namespace aether
{
	void TextRenderer::Init(ServiceContainer& services, std::string_view fontVfsPath, int glyphSize)
	{
		m_vkCtx = &services.Get<VulkanContext>();
		m_bindlessMgr = &services.Get<BindlessManager>();
		m_swapchain = &services.Get<Swapchain>();

		if (!io::FileSystem::Exists(fontVfsPath))
		{
			WARN(LogCategory::Asset,
			        "TextRenderer: font not found at '{}'. Text rendering disabled. "
			        "Place a .ttf file at that VFS path.",
			        fontVfsPath);
			return;
		}

		const VulkanContext& vk = *m_vkCtx;
		m_fontAtlas.Build(fontVfsPath, glyphSize, vk.GetDevice().device, vk.GetAllocator(), vk.GetGraphicsQueue(), vk.GetGraphicsQueueFamily(), *m_bindlessMgr);

		m_ready = true;
		INFO(LogCategory::Engine, "TextRenderer: font atlas ready.");
	}

	void TextRenderer::Shutdown()
	{
		m_fontAtlas.Destroy();
		m_vkCtx = nullptr;
		m_bindlessMgr = nullptr;
		m_swapchain = nullptr;
		m_ready = false;
	}

	void TextRenderer::DrawTextLayered(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color, std::int32_t layer, QuadRenderer& qr)
	{
		if (m_vkCtx == nullptr || !m_ready || !m_fontAtlas.IsValid() || text.empty())
		{
			return;
		}

		const glm::vec2 origin = ResolveUiPointPx(m_swapchain->GetExtent(), point);
		float cursorX = origin.x;
		const std::uint32_t atlasSlot = m_fontAtlas.GetBindlessSlot();

		for (char c: text)
		{
			const GlyphInfo& g = m_fontAtlas.GetGlyph(c);

			if (c == ' ' || g.width == 0.f || g.height == 0.f)
			{
				cursorX += g.advanceX * fontSize;
				continue;
			}

			const glm::vec4 glyphRectPx(cursorX + g.bearingX * fontSize, origin.y - g.bearingY * fontSize, g.width * fontSize, g.height * fontSize);

			qr.DrawGlyph(glyphRectPx, g.uvRect, color, atlasSlot, layer);

			cursorX += g.advanceX * fontSize;
		}
	}

	float TextRenderer::MeasureText(std::string_view text, float fontSize) const
	{
		if (!m_ready)
		{
			return 0.f;
		}
		float width = 0.f;
		for (char c: text)
		{
			width += m_fontAtlas.GetGlyph(c).advanceX * fontSize;
		}
		return width;
	}

} // namespace aether
