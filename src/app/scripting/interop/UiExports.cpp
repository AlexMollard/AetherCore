#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>

#include <entt/entt.hpp>

#include "assets/AssetManager.hpp"
#include "material/TextureHandle.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/CursorService.hpp"
#include "ui/UiEntities.hpp"
#include "ui/UiTextEdit.hpp"
#include "utils/ServiceContainer.hpp"

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

// Typography reaches EVERY element that draws glyphs, not only UIText. A text box and a
// button each carry their own font name and pixel size (they are separate components,
// not a UIText plus a frame), so a script that styled a label and then styled the field
// under it silently got half of what it asked for - and the two could not be kept in
// step from script at all. Written to whichever of the three the entity actually has;
// an entity carrying none is left alone, as every other Ui.* setter leaves it.
AE_SCRIPT_API void aether_ui_set_font_size(std::uint32_t id, float pixelSize)
{
	auto& world = ActiveWorld();
	const aether::Entity entity{id};
	if (auto* t = world.TryGet<aether::ui::UIText>(entity))
	{
		t->pixelSize = pixelSize;
	}
	if (auto* b = world.TryGet<aether::ui::UITextBox>(entity))
	{
		b->pixelSize = pixelSize;
	}
	if (auto* button = world.TryGet<aether::ui::UIButton>(entity))
	{
		button->pixelSize = pixelSize;
	}
}

AE_SCRIPT_API void aether_ui_set_font(std::uint32_t id, const char* name)
{
	auto& world = ActiveWorld();
	const aether::Entity entity{id};
	const std::string font = name != nullptr ? name : "";
	if (auto* t = world.TryGet<aether::ui::UIText>(entity))
	{
		t->fontName = font;
	}
	if (auto* b = world.TryGet<aether::ui::UITextBox>(entity))
	{
		b->fontName = font;
	}
	if (auto* button = world.TryGet<aether::ui::UIButton>(entity))
	{
		button->fontName = font;
	}
}

AE_SCRIPT_API void aether_ui_set_text_wrap(std::uint32_t id, std::int32_t wrap)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIText>(aether::Entity{id}))
	{
		t->wrap = wrap != 0;
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

// Nearest sampling. Small art blown up under the shared linear sampler turns to mush; the
// draw builder already honours this flag, there was just no way to ask for it from a script.
AE_SCRIPT_API void aether_ui_set_image_pixel_art(std::uint32_t id, std::int32_t enabled)
{
	auto* img = ActiveWorld().TryGet<aether::ui::UIImage>(aether::Entity{id});
	if (img != nullptr)
	{
		img->pixelArt = enabled != 0;
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

// Which element holds the keyboard right now, or 0 for nobody.
//
// Answered from the UI's own committed state, which UiNavigationSystem writes BEFORE the
// first script of the frame runs - so every script that asks gets the same answer in the
// same frame, whatever order they happen to update in. That is the whole point of it
// existing: a game whose character controller has to stand still while a menu or a chat
// field is up would otherwise have one script publish a flag for another to read, and
// which of the two ran first would decide whether the character walked.
AE_SCRIPT_API std::uint32_t aether_ui_focused_entity()
{
	std::uint32_t focused = 0;
	ActiveWorld().View<aether::ui::UISelectable>().each(
	        [&](entt::entity ent, aether::ui::UISelectable& s)
	        {
		        if (s.focused)
		        {
			        focused = aether::World::FromEntt(ent).id;
		        }
	        });
	return focused;
}

// Join (or leave) the navigation system at runtime.
//
// Focus, spatial navigation, click-and-key activation and controller support all key off
// UISelectable, which until now only the scene could author - so a menu built at runtime, or
// one assembled from plain images because it wanted its own look, could not be navigated at
// all and had to hand-roll an index and its own idea of "confirm". This is the one call that
// was missing to make that a solved problem everywhere rather than per game.
//
// Idempotent in both directions. An element with no UIRect simply never appears in the
// navigation view, so this is safe to call before layout has run.
AE_SCRIPT_API void aether_ui_set_selectable(std::uint32_t id, std::int32_t selectable)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	// A UIRect is what makes something a laid-out UI element, and the navigation view is keyed
	// on having one - so requiring it here is both the meaningful precondition and the guard
	// against being handed an entity that is not UI at all.
	if (world.TryGet<aether::ui::UIRect>(e) == nullptr)
	{
		return;
	}
	const bool has = world.TryGet<aether::ui::UISelectable>(e) != nullptr;
	if (selectable != 0 && !has)
	{
		world.Emplace<aether::ui::UISelectable>(e);
	}
	else if (selectable == 0 && has)
	{
		world.Remove<aether::ui::UISelectable>(e);
	}
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

// Focus is exclusive, and "nobody" is one of its legal values: UiNavigationSystem never
// picks an element on its own, so this survives instead of being re-stamped next frame.
AE_SCRIPT_API void aether_ui_clear_focus()
{
	ActiveWorld().View<aether::ui::UISelectable>().each([](entt::entity, aether::ui::UISelectable& s) { s.focused = false; });
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
		t->knobT = t->on ? 1.f : 0.f; // programmatic set snaps; only user flips animate
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

// ── Text box ────────────────────────────────────────────────────────────────
AE_SCRIPT_API std::uint32_t aether_ui_create_text_box(std::uint32_t canvasId)
{
	auto& world = ActiveWorld();
	return aether::ui::CreateTextBoxEntity(world, ResolveCanvas(world, canvasId)).id;
}

AE_SCRIPT_API std::int32_t aether_ui_get_text_box_text(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	if (b == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(b->text.size()));
	std::memcpy(buf, b->text.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_ui_set_text_box_text(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->text = text != nullptr ? text : "";
		// A programmatic set puts the caret at the end and drops any selection, so the next
		// keystroke appends instead of replacing text the player never chose.
		b->caret = static_cast<int>(b->text.size());
		b->selectionAnchor = b->caret;
		b->scrollX = 0.f;
	}
}

AE_SCRIPT_API void aether_ui_set_text_box_placeholder(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->placeholder = text != nullptr ? text : "";
	}
}

AE_SCRIPT_API void aether_ui_set_text_box_content_type(std::uint32_t id, std::int32_t contentType)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->contentType = static_cast<aether::ui::TextContentType>(contentType);
	}
}

AE_SCRIPT_API void aether_ui_set_text_box_max_length(std::uint32_t id, std::int32_t maxLength)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		// Negative is meaningless and 0 already means unlimited, so both collapse to
		// unlimited rather than to a field that refuses every keystroke.
		b->maxLength = maxLength > 0 ? maxLength : 0;
	}
}

AE_SCRIPT_API std::int32_t aether_ui_was_submitted(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->submitted) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_ui_was_cancelled(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->cancelled) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_ui_is_editing(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->editing) ? 1 : 0;
}

AE_SCRIPT_API void aether_ui_begin_edit(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	// `activated` cannot be used here: this export runs from a C# Update(), which fires after
	// UiNavigationSystem, UiWidgetSystem and UiTextBoxSystem have already run this frame. By the
	// time UiTextBoxSystem next runs, UiNavigationSystem will have already re-stamped `activated`
	// from live input for every UISelectable, clobbering whatever this call set. `pendingEdit` is
	// a one-shot channel UiTextBoxSystem consumes itself, so the entry logic (snapshot, select
	// all, capture) still lives in exactly one place - the system - rather than being duplicated
	// here.
	// Focus is exclusive, so it is cleared everywhere else rather than just set here - the same
	// thing aether_ui_set_focus does. Leaving a second element focused makes the next
	// UiNavigationSystem tick pick `prevFocused` by last-write-wins over an unspecified view
	// order: if the other one wins, the box enters editing and loses it one frame later.
	if (world.TryGet<aether::ui::UISelectable>(e) != nullptr)
	{
		world.View<aether::ui::UISelectable>().each([&](entt::entity ent, aether::ui::UISelectable& s) { s.focused = (aether::World::FromEntt(ent) == e); });
	}
	if (auto* box = world.TryGet<aether::ui::UITextBox>(e))
	{
		box->pendingEdit = true;
	}
}

// True if a UISlider, UIToggle or UITextBox on this entity changed by user input this frame.
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
	if (const auto* b = world.TryGet<aether::ui::UITextBox>(e); b != nullptr && b->changed)
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

AE_SCRIPT_API void aether_ui_set_effect_sort_order(std::uint32_t id, int sortOrder)
{
	if (auto* fx = ActiveWorld().TryGet<aether::ui::UIEffect>(aether::Entity{id}))
	{
		fx->sortOrder = sortOrder;
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

// ── Mouse pointer ─────────────────────────────────────────────────────────────
// The engine draws the pointer (see ui::CursorService) and the project configures it in
// ProjectSettings.toml. These are only for games that need MORE than one pointer - a pen while
// drawing, an arrow in menus - or that want it out of the way while they draw their own marker.

namespace
{
	aether::ui::CursorService* Cursor()
	{
		auto* services = ActiveContext().services;
		return services != nullptr ? services->TryGet<aether::ui::CursorService>() : nullptr;
	}
} // namespace

AE_SCRIPT_API void aether_cursor_set_visible(std::int32_t visible)
{
	if (auto* cursor = Cursor())
	{
		cursor->SetVisible(visible != 0);
	}
}

AE_SCRIPT_API std::int32_t aether_cursor_get_visible()
{
	auto* cursor = Cursor();
	return cursor != nullptr && cursor->IsVisible() ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_cursor_enabled()
{
	auto* cursor = Cursor();
	return cursor != nullptr && cursor->IsEnabled() ? 1 : 0;
}

AE_SCRIPT_API void aether_cursor_set_look(const char* texture, float hotspotX, float hotspotY, float size, std::int32_t pixelArt)
{
	if (auto* cursor = Cursor())
	{
		cursor->SetLook({
		        .texture = texture != nullptr ? texture : "",
		        .hotspot = {hotspotX, hotspotY},
		        .size = size,
		        .pixelArt = pixelArt != 0,
		});
	}
}

AE_SCRIPT_API void aether_cursor_reset()
{
	if (auto* cursor = Cursor())
	{
		cursor->Reset();
	}
}
