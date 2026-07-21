#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>

#include <entt/entt.hpp>

#include "assets/AssetManager.hpp"
#include "material/TextureHandle.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEntities.hpp"

// In-game UI control exported to C#. Text is read live each frame, and the layout

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	aether::Entity ResolveCanvas(aether::World& world, std::uint32_t canvasId)
	{
		const aether::Entity c{canvasId};
		if (c.IsValid() && world.Has<aether::ui::UICanvas>(c))
		{
			return c;
		}
		auto view = world.View<aether::ui::UICanvas>();
		if (auto it = view.begin(); it != view.end())
		{
			return aether::World::FromEntt(*it);
		}
		return aether::ui::CreateCanvasEntity(world);
	}
} // namespace

AE_SCRIPT_API void aether_ui_set_text(std::uint32_t id, const char* text)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id}))
	{
		t->text = text != nullptr ? text : "";
	}
}

AE_SCRIPT_API std::int32_t aether_ui_get_text(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id});
	if (t == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(t->text.size()));
	std::memcpy(buf, t->text.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API std::uint32_t aether_ui_create_canvas()
{
	return aether::ui::CreateCanvasEntity(ActiveWorld()).id;
}

AE_SCRIPT_API std::uint32_t aether_ui_create_image(std::uint32_t canvas)
{
	auto& world = ActiveWorld();
	return aether::ui::CreateImageEntity(world, ResolveCanvas(world, canvas)).id;
}

AE_SCRIPT_API std::uint32_t aether_ui_create_text(std::uint32_t canvas)
{
	auto& world = ActiveWorld();
	return aether::ui::CreateTextEntity(world, ResolveCanvas(world, canvas)).id;
}

// ── Layout (UIRect) ──────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_ui_set_anchors(std::uint32_t id, Vec2 min, Vec2 max)
{
	if (auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id}))
	{
		r->anchorMin = ToGlm(min);
		r->anchorMax = ToGlm(max);
	}
}

AE_SCRIPT_API void aether_ui_set_offsets(std::uint32_t id, Vec2 min, Vec2 max)
{
	if (auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id}))
	{
		r->offsetMin = ToGlm(min);
		r->offsetMax = ToGlm(max);
	}
}

AE_SCRIPT_API void aether_ui_set_pivot(std::uint32_t id, Vec2 pivot)
{
	if (auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id}))
	{
		r->pivot = ToGlm(pivot);
	}
}

AE_SCRIPT_API void aether_ui_set_rect(std::uint32_t id, float x, float y, float w, float h)
{
	if (auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id}))
	{
		r->offsetMin = {x - r->pivot.x * w, y - r->pivot.y * h};
		r->offsetMax = {x + (1.0f - r->pivot.x) * w, y + (1.0f - r->pivot.y) * h};
	}
}

AE_SCRIPT_API void aether_ui_set_text_color(std::uint32_t id, Vec4 color)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id}))
	{
		t->color = ToGlm(color);
	}
}

AE_SCRIPT_API void aether_ui_set_font_size(std::uint32_t id, float pixelSize)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id}))
	{
		t->pixelSize = pixelSize;
	}
}

AE_SCRIPT_API void aether_ui_set_text_align(std::uint32_t id, std::int32_t h, std::int32_t v)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id}))
	{
		t->hAlign = static_cast<aether::ui::UIText::HAlign>(std::clamp(h, 0, 2));
		t->vAlign = static_cast<aether::ui::UIText::VAlign>(std::clamp(v, 0, 2));
	}
}

AE_SCRIPT_API void aether_ui_set_image_color(std::uint32_t id, Vec4 color)
{
	if (auto* img = ActiveWorld().TryGet<aether::ui::UIImage>(aether::Entity{id}))
	{
		img->color = ToGlm(color);
	}
}

AE_SCRIPT_API void aether_ui_set_image_corner_radius(std::uint32_t id, float radius)
{
	if (auto* img = ActiveWorld().TryGet<aether::ui::UIImage>(aether::Entity{id}))
	{
		img->cornerRadius = radius;
	}
}

// back to a solid colour fill. The image holds the registry ref for its lifetime.
AE_SCRIPT_API void aether_ui_set_image_texture(std::uint32_t id, const char* path)
{
	auto& ctx = ActiveContext();
	auto* img = ActiveWorld().TryGet<aether::ui::UIImage>(aether::Entity{id});
	if (img == nullptr || ctx.assets == nullptr)
	{
		return;
	}
	if (path == nullptr || path[0] == '\0')
	{
		img->texture = aether::TextureHandle{};
		img->texturePath.clear();
		img->textureDirty = false;
		return;
	}
	img->texturePath = path;
	img->texture = ctx.assets->GetTextureRegistry().Acquire(path);
	img->textureDirty = false;
}

// ── UI selection / focus (driven by UiNavigationSystem) ─────────────────────────
AE_SCRIPT_API std::int32_t aether_ui_is_focused(std::uint32_t id)
{
	const auto* s = ActiveWorld().TryGet<aether::ui::UISelectable>(aether::Entity{id});
	return (s != nullptr && s->focused) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_ui_was_activated(std::uint32_t id)
{
	const auto* s = ActiveWorld().TryGet<aether::ui::UISelectable>(aether::Entity{id});
	return (s != nullptr && s->activated) ? 1 : 0;
}

AE_SCRIPT_API void aether_ui_set_focus(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity target{id};
	if (world.TryGet<aether::ui::UISelectable>(target) == nullptr)
	{
		return;
	}
	world.View<aether::ui::UISelectable>().each([&](entt::entity ent, aether::ui::UISelectable& s) { s.focused = (aether::World::FromEntt(ent) == target); });
}

AE_SCRIPT_API void aether_ui_set_interactable(std::uint32_t id, std::int32_t value)
{
	auto* s = ActiveWorld().TryGet<aether::ui::UISelectable>(aether::Entity{id});
	if (s != nullptr)
	{
		s->interactable = (value != 0);
	}
}

// ── Widgets ─────────────────────────────────────────────────────────────────────
AE_SCRIPT_API float aether_ui_get_slider_value(std::uint32_t id)
{
	const auto* s = ActiveWorld().TryGet<aether::ui::UISlider>(aether::Entity{id});
	return s != nullptr ? s->value : 0.f;
}

AE_SCRIPT_API void aether_ui_set_slider_value(std::uint32_t id, float value)
{
	if (auto* s = ActiveWorld().TryGet<aether::ui::UISlider>(aether::Entity{id}))
	{
		s->value = std::clamp(value, s->minValue, s->maxValue);
	}
}

AE_SCRIPT_API std::int32_t aether_ui_get_toggle(std::uint32_t id)
{
	const auto* t = ActiveWorld().TryGet<aether::ui::UIToggle>(aether::Entity{id});
	return (t != nullptr && t->on) ? 1 : 0;
}

AE_SCRIPT_API void aether_ui_set_toggle(std::uint32_t id, std::int32_t on)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIToggle>(aether::Entity{id}))
	{
		t->on = (on != 0);
	}
}

AE_SCRIPT_API float aether_ui_get_progress(std::uint32_t id)
{
	const auto* p = ActiveWorld().TryGet<aether::ui::UIProgressBar>(aether::Entity{id});
	return p != nullptr ? p->value : 0.f;
}

AE_SCRIPT_API void aether_ui_set_progress(std::uint32_t id, float value)
{
	if (auto* p = ActiveWorld().TryGet<aether::ui::UIProgressBar>(aether::Entity{id}))
	{
		p->value = std::clamp(value, 0.f, 1.f);
	}
}

AE_SCRIPT_API std::int32_t aether_ui_get_button_label(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UIButton>(aether::Entity{id});
	if (b == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(b->label.size()));
	std::memcpy(buf, b->label.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_ui_set_button_label(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UIButton>(aether::Entity{id}))
	{
		b->label = text != nullptr ? text : "";
	}
}

// True if a UISlider or UIToggle on this entity changed by user input this frame.
AE_SCRIPT_API std::int32_t aether_ui_was_changed(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (const auto* s = world.TryGet<aether::ui::UISlider>(e); s != nullptr && s->changed)
	{
		return 1;
	}
	if (const auto* t = world.TryGet<aether::ui::UIToggle>(e); t != nullptr && t->changed)
	{
		return 1;
	}
	return 0;
}

// ── Custom-shader effects (UIEffect, drawn by their own pipeline) ─────────────────
AE_SCRIPT_API std::uint32_t aether_ui_create_effect(std::uint32_t canvas, const char* shader)
{
	auto& world = ActiveWorld();
	return aether::ui::CreateEffectEntity(world, ResolveCanvas(world, canvas), shader != nullptr ? shader : "").id;
}

AE_SCRIPT_API void aether_ui_set_effect_params(std::uint32_t id, Vec4 params)
{
	if (auto* fx = ActiveWorld().TryGet<aether::ui::UIEffect>(aether::Entity{id}))
	{
		fx->params = ToGlm(params);
	}
}

AE_SCRIPT_API void aether_ui_set_effect_colors(std::uint32_t id, Vec4 color0, Vec4 color1)
{
	if (auto* fx = ActiveWorld().TryGet<aether::ui::UIEffect>(aether::Entity{id}))
	{
		fx->color0 = ToGlm(color0);
		fx->color1 = ToGlm(color1);
	}
}

// UIMaterial: a custom fragment shader on the element's own draw (glyph/quad-masked). set_material
// get-or-adds so a script can apply a shader to any UI element at runtime.
AE_SCRIPT_API void aether_ui_set_material(std::uint32_t id, const char* shader)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	auto& mat = world.Has<aether::ui::UIMaterial>(e) ? world.Get<aether::ui::UIMaterial>(e) : world.Emplace<aether::ui::UIMaterial>(e);
	mat.shader = shader != nullptr ? shader : "";
}

AE_SCRIPT_API void aether_ui_set_material_params(std::uint32_t id, Vec4 params)
{
	if (auto* mat = ActiveWorld().TryGet<aether::ui::UIMaterial>(aether::Entity{id}))
	{
		mat->params = ToGlm(params);
	}
}

AE_SCRIPT_API void aether_ui_set_material_colors(std::uint32_t id, Vec4 color0, Vec4 color1)
{
	if (auto* mat = ActiveWorld().TryGet<aether::ui::UIMaterial>(aether::Entity{id}))
	{
		mat->color0 = ToGlm(color0);
		mat->color1 = ToGlm(color1);
	}
}

// frame's layout pass. Useful for placing things relative to an element.
AE_SCRIPT_API Vec4 aether_ui_get_rect(std::uint32_t id)
{
	const auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id});
	if (r == nullptr)
	{
		return Vec4{};
	}
	return {r->resolvedRect.x, r->resolvedRect.y, r->resolvedRect.z, r->resolvedRect.w};
}

// layout resolves each frame, so this reflects the current on-screen position.
AE_SCRIPT_API std::int32_t aether_ui_contains_point(std::uint32_t id, Vec2 pt)
{
	const auto* r = ActiveWorld().TryGet<aether::ui::UIRect>(aether::Entity{id});
	if (r == nullptr)
	{
		return 0;
	}
	const glm::vec4 rect = r->resolvedRect;
	const bool inside = pt.x >= rect.x && pt.x <= rect.x + rect.z && pt.y >= rect.y && pt.y <= rect.y + rect.w;
	return inside ? 1 : 0;
}
