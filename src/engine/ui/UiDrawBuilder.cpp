#include "ui/UiDrawBuilder.hpp"

#include <algorithm>
#include <cmath>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "material/TextureRegistry.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	static void EmitImage(const UIRect& rect, const UIImage& img, const TextureRegistry* textures, int layer, std::vector<UiDrawCommand>& out)
	{
		UiDrawCommand cmd;
		cmd.data0 = rect.resolvedRect;
		cmd.color = img.color;
		cmd.layer = layer;
		if (img.texture.IsValid())
		{
			cmd.type = kShapeTexturedRect;
			cmd.data1 = {0.f, 0.f, 1.f, 1.f};
			cmd.textureSlot = textures != nullptr ? textures->ResolveSlot(img.texture) : 0xFFFFFFFFu;
			cmd.flags = img.pixelArt ? 1u : 0u;
		}
		else
		{
			cmd.type = kShapeRect;
			cmd.data1.x = img.cornerRadius;
		}
		out.push_back(cmd);
	}

	static void EmitTextRun(const glm::vec4& rect, const std::string& text, const std::string& fontName, float pixelSize, const glm::vec4& color, UIText::HAlign hAlign, UIText::VAlign vAlign, bool wrap, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const FontAsset* font = fonts.Load(fontName);
		if (font == nullptr || font->atlasBindlessSlot == 0xFFFFFFFFu)
		{
			return;
		}

		const std::vector<ShapedGlyph> glyphs = ShapeText(*font, text, pixelSize, rect, wrap, static_cast<int>(hAlign), static_cast<int>(vAlign));
		for (const ShapedGlyph& glyph: glyphs)
		{
			UiDrawCommand cmd;
			cmd.data0 = glyph.rect;
			cmd.data1 = glyph.uv;
			cmd.color = color;
			cmd.type = kShapeSdfGlyph;
			cmd.layer = layer++;
			cmd.textureSlot = font->atlasBindlessSlot;
			out.push_back(cmd);
		}
	}

	static void EmitText(const UIRect& rect, const UIText& text, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		EmitTextRun(rect.resolvedRect, text.text, text.fontName, text.pixelSize, text.color, text.hAlign, text.vAlign, text.wrap, fonts, layer, out);
	}

	static bool WidgetFocused(World& world, Entity e)
	{
		const auto* sel = world.TryGet<UISelectable>(e);
		return sel != nullptr && sel->focused;
	}

	static void EmitSlider(World& world, Entity e, const UIRect& rect, const UISlider& s, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;
		const float range = std::max(s.maxValue - s.minValue, 1e-6f);
		const float t = std::clamp((s.value - s.minValue) / range, 0.f, 1.f);
		const float pad = 2.f;
		const float innerX = r.x + pad, innerY = r.y + pad;
		const float innerW = std::max(r.z - 2.f * pad, 0.f), innerH = std::max(r.w - 2.f * pad, 0.f);
		const float fillW = innerW * t;

		UiDrawCommand track;
		track.type = kShapeRect;
		track.data0 = r;
		track.data1.x = s.cornerRadius;
		track.color = s.trackColor;
		track.layer = layer++;
		out.push_back(track);

		UiDrawCommand fill;
		fill.type = kShapeRect;
		fill.data0 = {innerX, innerY, std::max(fillW, 1.f), innerH};
		fill.data1.x = std::max(s.cornerRadius - pad, 0.f);
		fill.color = s.fillColor;
		fill.layer = layer++;
		out.push_back(fill);

		const bool focused = WidgetFocused(world, e);
		UiDrawCommand handle;
		handle.type = kShapeCircle;
		handle.data0 = {innerX + fillW, r.y + r.w * 0.5f, s.handleRadius + (focused ? 2.f : 0.f), 0.f};
		handle.color = s.handleColor;
		handle.color.a *= s.pulse;
		handle.layer = layer++;
		out.push_back(handle);
	}

	static void EmitToggle(World& world, Entity e, const UIRect& rect, const UIToggle& tg, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;

		UiDrawCommand track;
		track.type = kShapeRect;
		track.data0 = r;
		track.data1.x = std::min(tg.cornerRadius, r.w * 0.5f);
		track.color = tg.on ? tg.onColor : tg.trackColor;
		track.layer = layer++;
		out.push_back(track);

		const float pad = 2.f;
		const float leftX = r.x + pad + tg.knobRadius;
		const float rightX = r.x + r.z - pad - tg.knobRadius;
		const bool focused = WidgetFocused(world, e);
		UiDrawCommand knob;
		knob.type = kShapeCircle;
		knob.data0 = {tg.on ? rightX : leftX, r.y + r.w * 0.5f, tg.knobRadius + (focused ? 1.f : 0.f), 0.f};
		knob.color = tg.knobColor;
		knob.color.a *= tg.pulse;
		knob.layer = layer++;
		out.push_back(knob);
	}

	static void EmitProgressBar(const UIRect& rect, const UIProgressBar& p, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;

		UiDrawCommand track;
		track.type = kShapeRect;
		track.data0 = r;
		track.data1.x = p.cornerRadius;
		track.color = p.trackColor;
		track.layer = layer++;
		out.push_back(track);

		const float pad = 2.f;
		const float innerW = std::max(r.z - 2.f * pad, 0.f);
		const float t = std::clamp(p.value, 0.f, 1.f);
		UiDrawCommand fill;
		fill.type = kShapeRect;
		fill.data0 = {r.x + pad, r.y + pad, std::max(innerW * t, 1.f), std::max(r.w - 2.f * pad, 0.f)};
		fill.data1.x = std::max(p.cornerRadius - pad, 0.f);
		fill.color = p.fillColor;
		fill.layer = layer++;
		out.push_back(fill);
	}

	static void EmitButton(World& world, Entity e, const UIRect& rect, const UIButton& b, FontRegistry* fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const bool focused = WidgetFocused(world, e);
		UiDrawCommand bg;
		bg.type = kShapeRect;
		bg.data0 = rect.resolvedRect;
		bg.data1.x = b.cornerRadius;
		bg.color = focused ? b.bgColorFocused : b.bgColor;
		bg.layer = layer++;
		out.push_back(bg);
		if (fonts != nullptr)
		{
			EmitTextRun(rect.resolvedRect, b.label, b.fontName, b.pixelSize, focused ? b.textColorFocused : b.textColor, b.hAlign, b.vAlign, false, *fonts, layer, out);
		}
	}

	// shared across the whole canvas, so it must be threaded through by reference.
	static void Walk(World& world, Entity entity, FontRegistry* fonts, TextureRegistry* textures, int& layer, std::vector<UiDrawCommand>& out)
	{
		// walk simply stops recursing, so its children never emit either.
		if (world.Has<DisabledComponent>(entity))
		{
			return;
		}

		if (const auto* rect = world.TryGet<UIRect>(entity))
		{
			if (auto* img = world.TryGet<UIImage>(entity))
			{
				// Lazily resolve an authored texturePath to a handle (set via reflection/MCP,
				// serde, or the native setter). Done here because this is where a mutable
				// TextureRegistry meets the component each frame.
				if (img->textureDirty && textures != nullptr)
				{
					if (img->texture.IsValid())
					{
						textures->Release(img->texture);
					}
					img->texture = img->texturePath.empty() ? TextureHandle{} : textures->Acquire(img->texturePath);
					img->textureDirty = false;
				}
				EmitImage(*rect, *img, textures, layer++, out);
			}
			if (fonts != nullptr)
			{
				if (const auto* text = world.TryGet<UIText>(entity))
				{
					EmitText(*rect, *text, *fonts, layer, out);
				}
			}
			if (const auto* slider = world.TryGet<UISlider>(entity))
			{
				EmitSlider(world, entity, *rect, *slider, layer, out);
			}
			if (const auto* toggle = world.TryGet<UIToggle>(entity))
			{
				EmitToggle(world, entity, *rect, *toggle, layer, out);
			}
			if (const auto* bar = world.TryGet<UIProgressBar>(entity))
			{
				EmitProgressBar(*rect, *bar, layer, out);
			}
			if (fonts != nullptr)
			{
				if (const auto* button = world.TryGet<UIButton>(entity))
				{
					EmitButton(world, entity, *rect, *button, fonts, layer, out);
				}
			}
		}

		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
		{
			for (const Entity child: hierarchy->children)
			{
				Walk(world, child, fonts, textures, layer, out);
			}
		}
	}

	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out, FontRegistry* fonts, TextureRegistry* textures)
	{
		out.clear();
		int layer = 0;
		world.View<UICanvas>().each([&](entt::entity canvasEntity, UICanvas&) { Walk(world, World::FromEntt(canvasEntity), fonts, textures, layer, out); });
	}
} // namespace aether::ui
