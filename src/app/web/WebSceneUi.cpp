#include "WebScene.hpp"

#include <algorithm>
#include <cmath>
#include <emscripten.h>
#include <glm/gtc/matrix_transform.hpp>
#include "scene/Hierarchy.hpp"
#include "ui/UiComponents.hpp"
#include "utils/Utf8.hpp"

namespace aether::app::web
{
	Expected<void> WebScene::ExtractUi(RenderFramePacket& packet, unsigned width, unsigned height)
	{
		if (!width || !height)
		{
			return {};
		}
		if (!m_font_ready)
		{
			if (!EM_ASM_INT({ return aetherPlatform.fontReady; }))
			{
				return {};
			}
			std::vector<std::uint8_t> pixels(1024 * 1024 * 4);
			EM_ASM(
			        {
				        HEAPU8.set(aetherPlatform.fontAtlas, $0);
				        HEAPF32.set(aetherPlatform.advances, $1 / 4);
			        },
			        pixels.data(),
			        m_advances.data());
			if (!m_graphics.UploadTexture(255, 1024, 1024, pixels))
			{
				AE_UNEXPECTED(AetherError::Asset("Browser glyph atlas upload failed"));
			}
			m_font_ready = true;
		}
		glm::mat4 pixelsToClip(1);
		pixelsToClip[0][0] = 2.0f / width;
		pixelsToClip[1][1] = 2.0f / height;
		pixelsToClip[3] = {-1, -1, 0.5f, 1};
		const auto pixelsToWorld = glm::inverse(packet.proj * packet.view) * pixelsToClip;
		std::uint64_t order = UINT64_MAX - 65536;
		unsigned glyphs = 0;
		const auto quad = [&](glm::vec4 rect, glm::vec4 color, unsigned slot, glm::vec4 uv, Entity entity)
		{
			if (rect.z <= 0 || rect.w <= 0)
			{
				return;
			}
			packet.render2D.sprites.push_back({.world = pixelsToWorld * glm::translate(glm::mat4(1), {rect.x, rect.y, 0}),
			        .uvRect = uv,
			        .color = color,
			        .sizeAndPivot = {rect.z, rect.w, 0, 0},
			        .sortKey = order++,
			        .textureIndex = slot,
			        .entityId = entity.id,
			        .flags = static_cast<SpriteInstanceFlags>(static_cast<std::uint32_t>(SpriteInstanceFlags::FlipY) | static_cast<std::uint32_t>(SpriteInstanceFlags::DisplaySpace))});
		};
		const auto label = [&](const ui::UIText& text, glm::vec4 rect, Entity entity) -> Expected<void>
		{
			if (text.fontName != "Roboto" || !IsValidUtf8(text.text.data(), text.text.size()) || text.text.size() > 4096 || !std::isfinite(text.pixelSize) || text.pixelSize < 1 || text.pixelSize > 192)
			{
				AE_UNEXPECTED(AetherError::Asset("Unsupported browser font, text length or font size"));
			}
			const float scale = text.pixelSize / 48;
			std::vector<std::vector<unsigned>> lines(1);
			std::vector<float> widths(1);
			for (size_t index = 0; index < text.text.size();)
			{
				unsigned code = static_cast<unsigned char>(text.text[index++]);
				if (code >= 0xC2 && code <= 0xC3 && index < text.text.size())
				{
					code = ((code & 31) << 6) | (static_cast<unsigned char>(text.text[index++]) & 63);
				}
				if (code == '\n')
				{
					lines.emplace_back();
					widths.push_back(0);
					continue;
				}
				if (code < 32 || code >= 256 || (code >= 127 && code < 160))
				{
					AE_UNEXPECTED(AetherError::Asset("Browser glyph atlas supports Latin-1 text and newlines"));
				}
				const float advance = m_advances[code] * scale;
				if (text.wrap && !lines.back().empty() && widths.back() + advance > rect.z)
				{
					lines.emplace_back();
					widths.push_back(0);
				}
				lines.back().push_back(code);
				widths.back() += advance;
			}
			const float lineHeight = text.pixelSize * 1.3f;
			const float blockHeight = lines.size() * lineHeight;
			float y = rect.y + (text.vAlign == ui::UIText::VAlign::Bottom ? rect.w - blockHeight : text.vAlign == ui::UIText::VAlign::Middle ? (rect.w - blockHeight) * 0.5f : 0);
			for (size_t line = 0; line < lines.size(); ++line, y += lineHeight)
			{
				float x = rect.x + (text.hAlign == ui::UIText::HAlign::Right ? rect.z - widths[line] : text.hAlign == ui::UIText::HAlign::Center ? (rect.z - widths[line]) * 0.5f : 0);
				for (const auto code: lines[line])
				{
					const float size = 64 * scale;
					const glm::vec2 begin = glm::max(glm::vec2{x, y}, glm::vec2{rect});
					const glm::vec2 end = glm::min(glm::vec2{x + size, y + size}, glm::vec2{rect.x + rect.z, rect.y + rect.w});
					if (end.x > begin.x && end.y > begin.y)
					{
						const glm::vec2 cell{code % 16, code / 16};
						const glm::vec2 uvBegin = (cell + (begin - glm::vec2{x, y}) / size) / 16.0f;
						const glm::vec2 uvEnd = (cell + (end - glm::vec2{x, y}) / size) / 16.0f;
						quad({begin, end - begin}, text.color, 255, {uvBegin, uvEnd}, entity);
						++glyphs;
					}
					x += m_advances[code] * scale;
				}
			}
			return {};
		};

		// Match the native canvas bias/entity ordering and parent-before-child walk.
		std::vector<std::pair<int, Entity>> canvases;
		world.View<ui::UICanvas>().each([&](entt::entity raw, const ui::UICanvas& canvas) { canvases.emplace_back(canvas.sortBias, World::FromEntt(raw)); });
		std::sort(canvases.begin(), canvases.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second.id < b.second.id; });
		std::vector<Entity> ordered;
		const auto walk = [&](auto&& self, Entity entity) -> void
		{
			if (world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				return;
			}
			if (world.Has<ui::UIRect>(entity))
			{
				ordered.push_back(entity);
			}
			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			{
				for (const auto child: hierarchy->children)
				{
					self(self, child);
				}
			}
		};
		for (const auto& [bias, canvas]: canvases)
		{
			walk(walk, canvas);
		}
		for (const auto entity: ordered)
		{
			if (world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			const auto rect = world.Get<ui::UIRect>(entity).resolvedRect;
			if (auto* image = world.TryGet<ui::UIImage>(entity))
			{
				unsigned slot = 1;
				if (!image->texturePath.empty())
				{
					auto found = m_ui_textures.find(image->texturePath);
					if (found == m_ui_textures.end())
					{
						found = m_ui_textures.emplace(image->texturePath, m_textures.Acquire(image->texturePath)).first;
					}
					slot = m_textures.ResolveSlot(found->second);
				}
				quad(rect, image->color, slot, {0, 0, 1, 1}, entity);
			}
			const auto* selectable = world.TryGet<ui::UISelectable>(entity);
			const bool focused = selectable && selectable->focused;
			if (const auto* button = world.TryGet<ui::UIButton>(entity))
			{
				quad(rect, focused ? button->bgColorFocused : button->bgColor, 1, {0, 0, 1, 1}, entity);
				ui::UIText text;
				text.text = button->label;
				text.fontName = button->fontName;
				text.pixelSize = button->pixelSize;
				text.color = focused ? button->textColorFocused : button->textColor;
				text.hAlign = button->hAlign;
				text.vAlign = button->vAlign;
				AE_TRY_VOID(label(text, rect, entity));
			}
			if (const auto* meter = world.TryGet<ui::UIProgressBar>(entity))
			{
				quad(rect, meter->trackColor, 1, {0, 0, 1, 1}, entity);
				auto fill = rect;
				fill.z *= std::clamp(meter->value, 0.0f, 1.0f);
				quad(fill, meter->fillColor, 1, {0, 0, 1, 1}, entity);
			}
			if (const auto* slider = world.TryGet<ui::UISlider>(entity))
			{
				quad(rect, slider->trackColor, 1, {0, 0, 1, 1}, entity);
				auto fill = rect;
				fill.z *= std::clamp((slider->value - slider->minValue) / std::max(slider->maxValue - slider->minValue, 0.001f), 0.0f, 1.0f);
				quad(fill, slider->fillColor, 1, {0, 0, 1, 1}, entity);
				quad({fill.x + fill.z - 3, fill.y, 6, fill.w}, slider->handleColor, 1, {0, 0, 1, 1}, entity);
			}
			if (const auto* toggle = world.TryGet<ui::UIToggle>(entity))
			{
				quad(rect, toggle->on ? toggle->onColor : toggle->trackColor, 1, {0, 0, 1, 1}, entity);
				quad({rect.x + (toggle->on ? rect.z * 0.5f : 0), rect.y, rect.z * 0.5f, rect.w}, toggle->knobColor, 1, {0, 0, 1, 1}, entity);
			}
			if (const auto* text = world.TryGet<ui::UIText>(entity))
			{
				AE_TRY_VOID(label(*text, rect, entity));
			}
		}
		EM_ASM({ aetherRuntime.uiGlyphs = $0; }, glyphs);
		return {};
	}
} // namespace aether::app::web
