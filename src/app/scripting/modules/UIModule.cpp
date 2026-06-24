#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"

// -- Binding functions ---------------------------------------------------------

namespace
{
	using namespace aether::app::scripting;

	// -- Conversion helpers ----------------------------------------------------

	inline das::float4 ToDas(glm::vec4 v)
	{
		das::float4 r{};
		r.x = v.x;
		r.y = v.y;
		r.z = v.z;
		r.w = v.w;
		return r;
	}

	inline glm::vec4 ToGlm(das::float4 v)
	{
		return {v.x, v.y, v.z, v.w};
	}

	// Build an anchor-0 fixed pixel rect from top-left + size.
	inline aether::UiRect PixelRect(float x, float y, float w, float h)
	{
		return aether::UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {0.f, 0.f}, .offsetMinPx = {x, y}, .offsetMaxPx = {x + w, y + h}};
	}

	// -- Factory helpers -------------------------------------------------------
	// Convenience functions that create a fully-configured UI entity and return
	// its id. They push to sceneEntities so the engine cleans up on reload.

	// create_ui_panel(world, x, y, width, height, title) -> uint
	uint32_t das_create_ui_panel(aether::World* w, float x, float y, float width, float height, const char* title)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		auto& render = w->Emplace<aether::ui::UiRenderComponent>(e);
		render.backgroundColor = {0.08f, 0.10f, 0.11f, 0.95f};
		render.borderColor = {0.32f, 0.37f, 0.34f, 1.00f};
		render.cornerRadius = 3.f;
		render.borderWidth = 1.f;

		w->Emplace<aether::ui::UiInputComponent>(e);

		auto& panel = w->Emplace<aether::ui::UiPanelComponent>(e);
		panel.title = title ? title : "";
		panel.draggable = true;
		panel.expandedRect = transform.rect;

		w->Emplace<aether::ui::UiChildrenComponent>(e);

		return e.id;
	}

	// create_ui_button(world, x, y, width, height, label) -> uint
	uint32_t das_create_ui_button(aether::World* w, float x, float y, float width, float height, const char* label)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		w->Emplace<aether::ui::UiRenderComponent>(e);
		w->Emplace<aether::ui::UiInputComponent>(e);

		auto& btn = w->Emplace<aether::ui::UiButtonComponent>(e);
		btn.label = label ? label : "";

		return e.id;
	}

	// create_ui_label(world, x, y, width, height, text) -> uint
	uint32_t das_create_ui_label(aether::World* w, float x, float y, float width, float height, const char* text)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		auto& txt = w->Emplace<aether::ui::UiTextComponent>(e);
		txt.text = text ? text : "";

		return e.id;
	}

	// create_ui_slider(world, x, y, width, height, min, max, initial) -> uint
	uint32_t das_create_ui_slider(aether::World* w, float x, float y, float width, float height, float minVal, float maxVal, float initial)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		w->Emplace<aether::ui::UiRenderComponent>(e);
		w->Emplace<aether::ui::UiInputComponent>(e);

		auto& slider = w->Emplace<aether::ui::UiSliderComponent>(e);
		slider.min = minVal;
		slider.max = maxVal;
		slider.value = initial;

		return e.id;
	}

	// create_ui_checkbox(world, x, y, width, height, label, checked) -> uint
	uint32_t das_create_ui_checkbox(aether::World* w, float x, float y, float width, float height, const char* label, bool checked)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		w->Emplace<aether::ui::UiRenderComponent>(e);
		w->Emplace<aether::ui::UiInputComponent>(e);

		auto& cb = w->Emplace<aether::ui::UiCheckboxComponent>(e);
		cb.label = label ? label : "";
		cb.checked = checked;

		return e.id;
	}

	// create_ui_image(world, x, y, width, height, texture_slot) -> uint
	uint32_t das_create_ui_image(aether::World* w, float x, float y, float width, float height, uint32_t slot)
	{
		aether::Entity e = w->Create();
		ActiveContext().sceneEntities.push_back(e);

		auto& transform = w->Emplace<aether::ui::UiTransformComponent>(e);
		transform.rect = PixelRect(x, y, width, height);

		auto& img = w->Emplace<aether::ui::UiImageComponent>(e);
		img.textureSlot = slot;

		return e.id;
	}

	// add_ui_child(world, parent_id, child_id)
	// Adds child_id to the parent's UiChildrenComponent and sets UiParentComponent
	// on the child. Both components are created if absent.
	void das_add_ui_child(aether::World* w, uint32_t parentId, uint32_t childId)
	{
		auto parent = aether::Entity{parentId};
		auto child = aether::Entity{childId};

		if (!w->Has<aether::ui::UiChildrenComponent>(parent))
		{
			w->Emplace<aether::ui::UiChildrenComponent>(parent);
		}
		w->Get<aether::ui::UiChildrenComponent>(parent).children.push_back(child);

		if (!w->Has<aether::ui::UiParentComponent>(child))
		{
			w->Emplace<aether::ui::UiParentComponent>(child);
		}
		w->Get<aether::ui::UiParentComponent>(child).parent = parent;
	}

	// -- UiTransformComponent --------------------------------------------------

	void das_set_ui_rect(aether::World* w, uint32_t id, float anchorMinX, float anchorMinY, float anchorMaxX, float anchorMaxY, float offsetMinX, float offsetMinY, float offsetMaxX, float offsetMaxY)
	{
		auto c = w->TryGet<aether::ui::UiTransformComponent>(aether::Entity{id});
		if (!c)
		{
			return;
		}
		c->rect.anchorMin = {anchorMinX, anchorMinY};
		c->rect.anchorMax = {anchorMaxX, anchorMaxY};
		c->rect.offsetMinPx = {offsetMinX, offsetMinY};
		c->rect.offsetMaxPx = {offsetMaxX, offsetMaxY};
	}

	void das_set_ui_z_order(aether::World* w, uint32_t id, float z)
	{
		if (auto c = w->TryGet<aether::ui::UiTransformComponent>(aether::Entity{id}))
		{
			c->zOrder = z;
		}
	}

	void das_set_ui_flex_grow(aether::World* w, uint32_t id, float flex)
	{
		if (auto c = w->TryGet<aether::ui::UiTransformComponent>(aether::Entity{id}))
		{
			c->flexGrow = flex;
		}
	}

	// -- UiRenderComponent -----------------------------------------------------

	void das_set_ui_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id}))
		{
			c->backgroundColor = ToGlm(color);
		}
	}

	das::float4 das_get_ui_color(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id});
		return c ? ToDas(c->backgroundColor) : das::float4{};
	}

	void das_set_ui_border_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id}))
		{
			c->borderColor = ToGlm(color);
		}
	}

	void das_set_ui_corner_radius(aether::World* w, uint32_t id, float radius)
	{
		if (auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id}))
		{
			c->cornerRadius = radius;
		}
	}

	void das_set_ui_border_width(aether::World* w, uint32_t id, float width)
	{
		if (auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id}))
		{
			c->borderWidth = width;
		}
	}

	void das_set_ui_visible(aether::World* w, uint32_t id, bool visible)
	{
		if (auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id}))
		{
			c->visible = visible;
		}
	}

	bool das_get_ui_visible(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiRenderComponent>(aether::Entity{id});
		return c && c->visible;
	}

	// -- UiTextComponent -------------------------------------------------------

	void das_set_ui_text(aether::World* w, uint32_t id, const char* text)
	{
		if (auto c = w->TryGet<aether::ui::UiTextComponent>(aether::Entity{id}))
		{
			c->text = text ? text : "";
		}
	}

	const char* das_get_ui_text(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiTextComponent>(aether::Entity{id});
		return c ? c->text.c_str() : "";
	}

	void das_set_ui_font_size(aether::World* w, uint32_t id, float size)
	{
		if (auto c = w->TryGet<aether::ui::UiTextComponent>(aether::Entity{id}))
		{
			c->fontSize = size;
		}
	}

	void das_set_ui_text_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiTextComponent>(aether::Entity{id}))
		{
			c->color = ToGlm(color);
		}
	}

	// -- UiInputComponent (read-only - written by UiSystem each frame) ---------

	bool das_get_ui_hovered(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c && c->hovered;
	}

	bool das_get_ui_pressed(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c && c->pressed;
	}

	bool das_get_ui_clicked(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c && c->clicked;
	}

	bool das_get_ui_focused(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c && c->focused;
	}

	float das_get_ui_hover_t(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c ? c->hoverT : 0.f;
	}

	float das_get_ui_press_t(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiInputComponent>(aether::Entity{id});
		return c ? c->pressT : 0.f;
	}

	// -- UiPanelComponent ------------------------------------------------------

	void das_set_ui_panel_title(aether::World* w, uint32_t id, const char* title)
	{
		if (auto c = w->TryGet<aether::ui::UiPanelComponent>(aether::Entity{id}))
		{
			c->title = title ? title : "";
		}
	}

	const char* das_get_ui_panel_title(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiPanelComponent>(aether::Entity{id});
		return c ? c->title.c_str() : "";
	}

	void das_set_ui_panel_draggable(aether::World* w, uint32_t id, bool draggable)
	{
		if (auto c = w->TryGet<aether::ui::UiPanelComponent>(aether::Entity{id}))
		{
			c->draggable = draggable;
		}
	}

	bool das_get_ui_panel_collapsed(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiPanelComponent>(aether::Entity{id});
		return c && c->collapsed;
	}

	void das_set_ui_panel_collapsible(aether::World* w, uint32_t id, bool collapsible)
	{
		if (auto c = w->TryGet<aether::ui::UiPanelComponent>(aether::Entity{id}))
		{
			c->collapsible = collapsible;
		}
	}

	// -- UiButtonComponent -----------------------------------------------------

	void das_set_ui_button_label(aether::World* w, uint32_t id, const char* label)
	{
		if (auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id}))
		{
			c->label = label ? label : "";
		}
	}

	const char* das_get_ui_button_label(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id});
		return c ? c->label.c_str() : "";
	}

	void das_set_ui_button_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id}))
		{
			c->normalColor = ToGlm(color);
		}
	}

	void das_set_ui_button_hover_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id}))
		{
			c->hoverColor = ToGlm(color);
		}
	}

	void das_set_ui_button_press_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id}))
		{
			c->pressColor = ToGlm(color);
		}
	}

	void das_set_ui_button_text_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiButtonComponent>(aether::Entity{id}))
		{
			c->textColor = ToGlm(color);
		}
	}

	// -- UiSliderComponent -----------------------------------------------------

	float das_get_ui_slider_value(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id});
		return c ? c->value : 0.f;
	}

	void das_set_ui_slider_value(aether::World* w, uint32_t id, float value)
	{
		if (auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id}))
		{
			c->value = value;
		}
	}

	void das_set_ui_slider_range(aether::World* w, uint32_t id, float min, float max)
	{
		if (auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id}))
		{
			c->min = min;
			c->max = max;
		}
	}

	const char* das_get_ui_slider_label(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id});
		return c ? c->label.c_str() : "";
	}

	bool das_get_ui_slider_dragging(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id});
		return c && c->isDragging;
	}

	void das_set_ui_slider_label(aether::World* w, uint32_t id, const char* label)
	{
		if (auto c = w->TryGet<aether::ui::UiSliderComponent>(aether::Entity{id}))
		{
			c->label = label ? label : "";
		}
	}

	// -- UiCheckboxComponent ---------------------------------------------------

	bool das_get_ui_checked(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiCheckboxComponent>(aether::Entity{id});
		return c && c->checked;
	}

	void das_set_ui_checked(aether::World* w, uint32_t id, bool checked)
	{
		if (auto c = w->TryGet<aether::ui::UiCheckboxComponent>(aether::Entity{id}))
		{
			c->checked = checked;
		}
	}

	void das_set_ui_checkbox_label(aether::World* w, uint32_t id, const char* label)
	{
		if (auto c = w->TryGet<aether::ui::UiCheckboxComponent>(aether::Entity{id}))
		{
			c->label = label ? label : "";
		}
	}

	const char* das_get_ui_checkbox_label(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiCheckboxComponent>(aether::Entity{id});
		return c ? c->label.c_str() : "";
	}

	// -- UiImageComponent ------------------------------------------------------

	void das_set_ui_image_slot(aether::World* w, uint32_t id, uint32_t slot)
	{
		if (auto c = w->TryGet<aether::ui::UiImageComponent>(aether::Entity{id}))
		{
			c->textureSlot = slot;
		}
	}

	void das_set_ui_image_tint(aether::World* w, uint32_t id, das::float4 tint)
	{
		if (auto c = w->TryGet<aether::ui::UiImageComponent>(aether::Entity{id}))
		{
			c->tint = ToGlm(tint);
		}
	}

	void das_set_ui_image_uv(aether::World* w, uint32_t id, float u0, float v0, float u1, float v1)
	{
		if (auto c = w->TryGet<aether::ui::UiImageComponent>(aether::Entity{id}))
		{
			c->uvRect = {u0, v0, u1, v1};
		}
	}

	// -- UiLayoutComponent -----------------------------------------------------

	void das_set_ui_layout_vertical(aether::World* w, uint32_t id)
	{
		if (auto c = w->TryGet<aether::ui::UiLayoutComponent>(aether::Entity{id}))
		{
			c->direction = aether::ui::UiLayoutComponent::Direction::Vertical;
		}
	}

	void das_set_ui_layout_horizontal(aether::World* w, uint32_t id)
	{
		if (auto c = w->TryGet<aether::ui::UiLayoutComponent>(aether::Entity{id}))
		{
			c->direction = aether::ui::UiLayoutComponent::Direction::Horizontal;
		}
	}

	void das_set_ui_layout_spacing(aether::World* w, uint32_t id, float spacing)
	{
		if (auto c = w->TryGet<aether::ui::UiLayoutComponent>(aether::Entity{id}))
		{
			c->spacing = spacing;
		}
	}

	void das_set_ui_layout_padding(aether::World* w, uint32_t id, float padding)
	{
		if (auto c = w->TryGet<aether::ui::UiLayoutComponent>(aether::Entity{id}))
		{
			c->padding = padding;
		}
	}

	void das_set_ui_auto_size(aether::World* w, uint32_t id, bool autoSize)
	{
		if (auto c = w->TryGet<aether::ui::UiLayoutComponent>(aether::Entity{id}))
		{
			c->autoSize = autoSize;
		}
	}

	// -- UiTextInputComponent --------------------------------------------------

	const char* das_get_ui_input_text(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiTextInputComponent>(aether::Entity{id});
		return c ? c->text.c_str() : "";
	}

	void das_set_ui_input_text(aether::World* w, uint32_t id, const char* text)
	{
		if (auto c = w->TryGet<aether::ui::UiTextInputComponent>(aether::Entity{id}))
		{
			c->text = text ? text : "";
			c->cursorPos = static_cast<int>(c->text.size());
		}
	}

	const char* das_get_ui_input_placeholder(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiTextInputComponent>(aether::Entity{id});
		return c ? c->placeholder.c_str() : "";
	}

	void das_set_ui_input_placeholder(aether::World* w, uint32_t id, const char* text)
	{
		if (auto c = w->TryGet<aether::ui::UiTextInputComponent>(aether::Entity{id}))
		{
			c->placeholder = text ? text : "";
		}
	}

	bool das_get_ui_input_submitted(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiTextInputComponent>(aether::Entity{id});
		return c && c->submitted;
	}

	// -- UiGridLayoutComponent -------------------------------------------------

	void das_set_ui_grid_columns(aether::World* w, uint32_t id, int columns)
	{
		if (auto c = w->TryGet<aether::ui::UiGridLayoutComponent>(aether::Entity{id}))
		{
			c->columns = columns;
		}
	}

	void das_set_ui_grid_slot_size(aether::World* w, uint32_t id, float size)
	{
		if (auto c = w->TryGet<aether::ui::UiGridLayoutComponent>(aether::Entity{id}))
		{
			c->slotSize = size;
		}
	}

	void das_set_ui_grid_spacing(aether::World* w, uint32_t id, float spacing)
	{
		if (auto c = w->TryGet<aether::ui::UiGridLayoutComponent>(aether::Entity{id}))
		{
			c->spacing = spacing;
		}
	}

	// -- UiItemSlotComponent ---------------------------------------------------

	void das_set_ui_item_slot(aether::World* w, uint32_t id, uint32_t textureSlot, int quantity)
	{
		if (auto c = w->TryGet<aether::ui::UiItemSlotComponent>(aether::Entity{id}))
		{
			c->textureSlot = textureSlot;
			c->quantity = quantity;
		}
	}

	int das_get_ui_item_quantity(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiItemSlotComponent>(aether::Entity{id});
		return c ? c->quantity : 0;
	}

	bool das_get_ui_item_selected(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiItemSlotComponent>(aether::Entity{id});
		return c && c->selected;
	}

	void das_set_ui_item_rarity_color(aether::World* w, uint32_t id, das::float4 color)
	{
		if (auto c = w->TryGet<aether::ui::UiItemSlotComponent>(aether::Entity{id}))
		{
			c->rarityColor = ToGlm(color);
		}
	}

	// -- UiParentComponent -----------------------------------------------------

	uint32_t das_get_ui_parent(aether::World* w, uint32_t id)
	{
		const auto c = w->TryGet<aether::ui::UiParentComponent>(aether::Entity{id});
		return c ? c->parent.id : 0u;
	}

	void das_set_ui_parent(aether::World* w, uint32_t id, uint32_t parentId)
	{
		if (auto c = w->TryGet<aether::ui::UiParentComponent>(aether::Entity{id}))
		{
			c->parent = aether::Entity{parentId};
		}
	}

} // namespace

// -- Module --------------------------------------------------------------------

namespace aether::app::scripting
{
	struct UIModule : DasModuleBase
	{
		UIModule()
		      : DasModuleBase("ui")
		{
			das::ModuleLibrary lib(this);
			lib.addModule(das::Module::require("world")); // World* used in all bindings

			// -- Factory helpers -----------------------------------------------
			Bind<das_create_ui_panel>(lib, "create_ui_panel", SE::modifyExternal);
			Bind<das_create_ui_button>(lib, "create_ui_button", SE::modifyExternal);
			Bind<das_create_ui_label>(lib, "create_ui_label", SE::modifyExternal);
			Bind<das_create_ui_slider>(lib, "create_ui_slider", SE::modifyExternal);
			Bind<das_create_ui_checkbox>(lib, "create_ui_checkbox", SE::modifyExternal);
			Bind<das_create_ui_image>(lib, "create_ui_image", SE::modifyExternal);
			Bind<das_add_ui_child>(lib, "add_ui_child", SE::modifyExternal);

			// -- UiTransformComponent ------------------------------------------
			BIND_COMPONENT("ui_transform", aether::ui::UiTransformComponent)
			Bind<das_set_ui_rect>(lib, "set_ui_rect", SE::modifyExternal);
			Bind<das_set_ui_z_order>(lib, "set_ui_z_order", SE::modifyExternal);
			Bind<das_set_ui_flex_grow>(lib, "set_ui_flex_grow", SE::modifyExternal);

			// -- UiRenderComponent ---------------------------------------------
			BIND_COMPONENT("ui_render", aether::ui::UiRenderComponent)
			Bind<das_set_ui_color>(lib, "set_ui_color", SE::modifyExternal);
			Bind<das_get_ui_color>(lib, "get_ui_color", SE::accessExternal);
			Bind<das_set_ui_border_color>(lib, "set_ui_border_color", SE::modifyExternal);
			Bind<das_set_ui_corner_radius>(lib, "set_ui_corner_radius", SE::modifyExternal);
			Bind<das_set_ui_border_width>(lib, "set_ui_border_width", SE::modifyExternal);
			Bind<das_set_ui_visible>(lib, "set_ui_visible", SE::modifyExternal);
			Bind<das_get_ui_visible>(lib, "get_ui_visible", SE::accessExternal);

			// -- UiTextComponent -----------------------------------------------
			BIND_COMPONENT("ui_text", aether::ui::UiTextComponent)
			Bind<das_set_ui_text>(lib, "set_ui_text", SE::modifyExternal);
			Bind<das_get_ui_text>(lib, "get_ui_text", SE::accessExternal);
			Bind<das_set_ui_font_size>(lib, "set_ui_font_size", SE::modifyExternal);
			Bind<das_set_ui_text_color>(lib, "set_ui_text_color", SE::modifyExternal);

			// -- UiInputComponent ----------------------------------------------
			BIND_COMPONENT("ui_input", aether::ui::UiInputComponent)
			Bind<das_get_ui_hovered>(lib, "get_ui_hovered", SE::accessExternal);
			Bind<das_get_ui_pressed>(lib, "get_ui_pressed", SE::accessExternal);
			Bind<das_get_ui_clicked>(lib, "get_ui_clicked", SE::accessExternal);
			Bind<das_get_ui_focused>(lib, "get_ui_focused", SE::accessExternal);
			Bind<das_get_ui_hover_t>(lib, "get_ui_hover_t", SE::accessExternal);
			Bind<das_get_ui_press_t>(lib, "get_ui_press_t", SE::accessExternal);

			// -- UiPanelComponent ----------------------------------------------
			BIND_COMPONENT("ui_panel", aether::ui::UiPanelComponent)
			Bind<das_set_ui_panel_title>(lib, "set_ui_panel_title", SE::modifyExternal);
			Bind<das_get_ui_panel_title>(lib, "get_ui_panel_title", SE::accessExternal);
			Bind<das_set_ui_panel_draggable>(lib, "set_ui_panel_draggable", SE::modifyExternal);
			Bind<das_set_ui_panel_collapsible>(lib, "set_ui_panel_collapsible", SE::modifyExternal);
			Bind<das_get_ui_panel_collapsed>(lib, "get_ui_panel_collapsed", SE::accessExternal);

			// -- UiButtonComponent ---------------------------------------------
			BIND_COMPONENT("ui_button", aether::ui::UiButtonComponent)
			Bind<das_set_ui_button_label>(lib, "set_ui_button_label", SE::modifyExternal);
			Bind<das_get_ui_button_label>(lib, "get_ui_button_label", SE::accessExternal);
			Bind<das_set_ui_button_color>(lib, "set_ui_button_color", SE::modifyExternal);
			Bind<das_set_ui_button_hover_color>(lib, "set_ui_button_hover_color", SE::modifyExternal);
			Bind<das_set_ui_button_press_color>(lib, "set_ui_button_press_color", SE::modifyExternal);
			Bind<das_set_ui_button_text_color>(lib, "set_ui_button_text_color", SE::modifyExternal);

			// -- UiSliderComponent ---------------------------------------------
			BIND_COMPONENT("ui_slider", aether::ui::UiSliderComponent)
			Bind<das_get_ui_slider_value>(lib, "get_ui_slider_value", SE::accessExternal);
			Bind<das_set_ui_slider_value>(lib, "set_ui_slider_value", SE::modifyExternal);
			Bind<das_set_ui_slider_range>(lib, "set_ui_slider_range", SE::modifyExternal);
			Bind<das_get_ui_slider_label>(lib, "get_ui_slider_label", SE::accessExternal);
			Bind<das_set_ui_slider_label>(lib, "set_ui_slider_label", SE::modifyExternal);
			Bind<das_get_ui_slider_dragging>(lib, "get_ui_slider_dragging", SE::accessExternal);

			// -- UiCheckboxComponent -------------------------------------------
			BIND_COMPONENT("ui_checkbox", aether::ui::UiCheckboxComponent)
			Bind<das_get_ui_checked>(lib, "get_ui_checked", SE::accessExternal);
			Bind<das_set_ui_checked>(lib, "set_ui_checked", SE::modifyExternal);
			Bind<das_set_ui_checkbox_label>(lib, "set_ui_checkbox_label", SE::modifyExternal);
			Bind<das_get_ui_checkbox_label>(lib, "get_ui_checkbox_label", SE::accessExternal);

			// -- UiImageComponent ----------------------------------------------
			BIND_COMPONENT("ui_image", aether::ui::UiImageComponent)
			Bind<das_set_ui_image_slot>(lib, "set_ui_image_slot", SE::modifyExternal);
			Bind<das_set_ui_image_tint>(lib, "set_ui_image_tint", SE::modifyExternal);
			Bind<das_set_ui_image_uv>(lib, "set_ui_image_uv", SE::modifyExternal);

			// -- UiLayoutComponent ---------------------------------------------
			BIND_COMPONENT("ui_layout", aether::ui::UiLayoutComponent)
			Bind<das_set_ui_layout_vertical>(lib, "set_ui_layout_vertical", SE::modifyExternal);
			Bind<das_set_ui_layout_horizontal>(lib, "set_ui_layout_horizontal", SE::modifyExternal);
			Bind<das_set_ui_layout_spacing>(lib, "set_ui_layout_spacing", SE::modifyExternal);
			Bind<das_set_ui_layout_padding>(lib, "set_ui_layout_padding", SE::modifyExternal);
			Bind<das_set_ui_auto_size>(lib, "set_ui_auto_size", SE::modifyExternal);

			// -- UiTextInputComponent ------------------------------------------
			BIND_COMPONENT("ui_text_input", aether::ui::UiTextInputComponent)
			Bind<das_get_ui_input_text>(lib, "get_ui_input_text", SE::accessExternal);
			Bind<das_set_ui_input_text>(lib, "set_ui_input_text", SE::modifyExternal);
			Bind<das_get_ui_input_placeholder>(lib, "get_ui_input_placeholder", SE::accessExternal);
			Bind<das_set_ui_input_placeholder>(lib, "set_ui_input_placeholder", SE::modifyExternal);
			Bind<das_get_ui_input_submitted>(lib, "get_ui_input_submitted", SE::accessExternal);

			// -- UiGridLayoutComponent -----------------------------------------
			BIND_COMPONENT("ui_grid_layout", aether::ui::UiGridLayoutComponent)
			Bind<das_set_ui_grid_columns>(lib, "set_ui_grid_columns", SE::modifyExternal);
			Bind<das_set_ui_grid_slot_size>(lib, "set_ui_grid_slot_size", SE::modifyExternal);
			Bind<das_set_ui_grid_spacing>(lib, "set_ui_grid_spacing", SE::modifyExternal);

			// -- UiItemSlotComponent -------------------------------------------
			BIND_COMPONENT("ui_item_slot", aether::ui::UiItemSlotComponent)
			Bind<das_set_ui_item_slot>(lib, "set_ui_item_slot", SE::modifyExternal);
			Bind<das_get_ui_item_quantity>(lib, "get_ui_item_quantity", SE::accessExternal);
			Bind<das_get_ui_item_selected>(lib, "get_ui_item_selected", SE::accessExternal);
			Bind<das_set_ui_item_rarity_color>(lib, "set_ui_item_rarity_color", SE::modifyExternal);

			// -- UiClipComponent -----------------------------------------------
			BIND_COMPONENT("ui_clip", aether::ui::UiClipComponent)

			// -- UiChildrenComponent / UiParentComponent -----------------------
			BIND_COMPONENT("ui_children", aether::ui::UiChildrenComponent)
			BIND_COMPONENT("ui_parent", aether::ui::UiParentComponent)
			Bind<das_get_ui_parent>(lib, "get_ui_parent", SE::accessExternal);
			Bind<das_set_ui_parent>(lib, "set_ui_parent", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(UIModule, aether::app::scripting)
