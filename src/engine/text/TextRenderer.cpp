#include "text/TextRenderer.hpp"

#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"
#include "ui/QuadRenderer.hpp"

namespace aether
{
	void TextRenderer::Init(ServiceContainer& services, std::string_view fontVfsPath, int glyphSize)
	{
		m_bindlessMgr = &services.Get<BindlessManager>();
		m_gpu = &services.Get<GpuDevice>();
		GpuDevice& gpu = *m_gpu;

		if (!io::FileSystem::Exists(fontVfsPath))
		{
			AE_WARN(LogCategory::Asset,
			        "TextRenderer: font not found at '{}'. Text rendering disabled. "
			        "Place a .ttf file at that VFS path.",
			        fontVfsPath);
			return;
		}

		// GpuDevice exposes opaque gpu::Device / gpu::Queue accessors.
		// The FontAtlas (and any other engine-side consumer)
		// never sees raw Vk* types.
		m_fontAtlas.Build(fontVfsPath, glyphSize, gpu.GetDevice(), gpu.GetGraphicsQueue(), gpu.GetGraphicsQueueFamily(), *m_bindlessMgr);

		m_ready = true;
		AE_INFO(LogCategory::Engine, "TextRenderer: font atlas ready.");
	}

	void TextRenderer::Shutdown()
	{
		m_fontAtlas.Destroy();
		m_bindlessMgr = nullptr;
		m_gpu = nullptr;
		m_ready = false;
	}

	void TextRenderer::DrawTextLayered(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color, std::int32_t layer, QuadRenderer& qr)
	{
		if (m_gpu == nullptr || !m_ready || !m_fontAtlas.IsValid() || text.empty())
		{
			return;
		}

		const glm::vec2 origin = ResolveUiPointPx(m_gpu->GetSwapchainExtent(), point);
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
