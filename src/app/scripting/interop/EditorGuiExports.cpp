// Immediate-mode ImGui bridge for project IEditorWindow tools. Editor-only: this TU is EXCLUDED from
// GameRuntime (which links no ImGui) via src/app/CMakeLists.txt, so a shipped build never requires
// ImGui. Each function is a thin wrapper over the live ImGui context, which is valid because the
// editor calls DrawEditorWindows synchronously inside its own ImGui frame on the main thread.

#include "scripting/interop/InteropCommon.hpp"

#include <imgui.h>

#include <string>

using namespace aether::app::scripting::interop;

namespace
{
	inline ImVec2 V2(Vec2 v) { return {v.x, v.y}; }
	inline ImVec4 V4(Vec4 v) { return {v.x, v.y, v.z, v.w}; }
	inline ImU32 U32(Vec4 v) { return ImGui::ColorConvertFloat4ToU32(V4(v)); }

	// Semantic theme slots -> the editor's LIVE ImGui style colors, so project windows follow whatever
	// theme is active. Keeps C# decoupled from raw ImGuiCol_ index values.
	ImVec4 ThemeColor(std::int32_t id)
	{
		switch (id)
		{
			case 0: return ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
			case 1: return ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
			case 2: return ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
			case 3: return ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
			case 4: return ImGui::GetStyleColorVec4(ImGuiCol_Header);
			case 5: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
			case 6: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
			case 7: return ImGui::GetStyleColorVec4(ImGuiCol_Border);
			case 8: return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);      // Accent (brand)
			case 9: return ImGui::GetStyleColorVec4(ImGuiCol_Button);        // AccentDim
			case 10: return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);    // Link
			default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
		}
	}
} // namespace

// ── Windows / layout ────────────────────────────────────────────────────────
// Initial size the next window opens at (FirstUseEver: the user can still resize + it persists).
AE_SCRIPT_API void aether_editorgui_set_next_window_size(Vec2 size) { ImGui::SetNextWindowSize(V2(size), ImGuiCond_FirstUseEver); }

AE_SCRIPT_API std::int32_t aether_editorgui_begin(const char* title, std::int32_t* open)
{
	bool o = open != nullptr ? (*open != 0) : true;
	const bool visible = ImGui::Begin(title, open != nullptr ? &o : nullptr);
	if (open != nullptr)
	{
		*open = o ? 1 : 0;
	}
	return visible ? 1 : 0;
}
AE_SCRIPT_API void aether_editorgui_end() { ImGui::End(); }

AE_SCRIPT_API std::int32_t aether_editorgui_begin_child(const char* id, Vec2 size, std::int32_t border)
{
	return ImGui::BeginChild(id, V2(size), border != 0) ? 1 : 0;
}
AE_SCRIPT_API void aether_editorgui_end_child() { ImGui::EndChild(); }

AE_SCRIPT_API void aether_editorgui_same_line() { ImGui::SameLine(); }
// Width of the next framed widget (combo/input/etc.); <0 means "fill to the right edge".
AE_SCRIPT_API void aether_editorgui_set_next_item_width(float w) { ImGui::SetNextItemWidth(w); }
// Height of a standard framed widget (font + frame padding) - for centering custom draw-list layout.
AE_SCRIPT_API float aether_editorgui_frame_height() { return ImGui::GetFrameHeight(); }
AE_SCRIPT_API Vec2 aether_editorgui_calc_text_size(const char* s) { const ImVec2 v = ImGui::CalcTextSize(s != nullptr ? s : ""); return {v.x, v.y}; }
// Clip subsequent draw-list drawing to a rect (e.g. a node's interior so text never spills). Must be
// balanced with pop_clip_rect.
AE_SCRIPT_API void aether_editorgui_push_clip_rect(Vec2 mn, Vec2 mx, std::int32_t intersect) { ImGui::GetWindowDrawList()->PushClipRect(V2(mn), V2(mx), intersect != 0); }
AE_SCRIPT_API void aether_editorgui_pop_clip_rect() { ImGui::GetWindowDrawList()->PopClipRect(); }
AE_SCRIPT_API void aether_editorgui_separator() { ImGui::Separator(); }
AE_SCRIPT_API void aether_editorgui_spacing() { ImGui::Spacing(); }
AE_SCRIPT_API Vec2 aether_editorgui_content_avail()
{
	const ImVec2 a = ImGui::GetContentRegionAvail();
	return {a.x, a.y};
}
AE_SCRIPT_API Vec2 aether_editorgui_cursor_screen_pos()
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	return {p.x, p.y};
}
AE_SCRIPT_API void aether_editorgui_set_cursor_screen_pos(Vec2 p) { ImGui::SetCursorScreenPos(V2(p)); }

// ── Text / widgets ──────────────────────────────────────────────────────────
AE_SCRIPT_API void aether_editorgui_text(const char* s) { ImGui::TextUnformatted(s); }
AE_SCRIPT_API void aether_editorgui_text_colored(Vec4 col, const char* s) { ImGui::TextColored(V4(col), "%s", s); }
AE_SCRIPT_API std::int32_t aether_editorgui_button(const char* label, Vec2 size) { return ImGui::Button(label, V2(size)) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_small_button(const char* label) { return ImGui::SmallButton(label) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_checkbox(const char* label, std::int32_t* v)
{
	bool b = v != nullptr && *v != 0;
	const bool changed = ImGui::Checkbox(label, &b);
	if (v != nullptr)
	{
		*v = b ? 1 : 0;
	}
	return changed ? 1 : 0;
}
AE_SCRIPT_API std::int32_t aether_editorgui_input_text(const char* label, char* buf, std::int32_t bufLen)
{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	return ImGui::InputText(label, buf, static_cast<std::size_t>(bufLen)) ? 1 : 0;
}
AE_SCRIPT_API std::int32_t aether_editorgui_input_text_multiline(const char* label, char* buf, std::int32_t bufLen, Vec2 size)
{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	return ImGui::InputTextMultiline(label, buf, static_cast<std::size_t>(bufLen), V2(size)) ? 1 : 0;
}
AE_SCRIPT_API std::int32_t aether_editorgui_input_float(const char* label, float* v) { return ImGui::InputFloat(label, v) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_selectable(const char* label, std::int32_t selected) { return ImGui::Selectable(label, selected != 0) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_tree_node(const char* label) { return ImGui::TreeNode(label) ? 1 : 0; }
AE_SCRIPT_API void aether_editorgui_tree_pop() { ImGui::TreePop(); }

// Combo over a '\n'-joined items string (managed joins string[] with '\n'); returns 1 on change.
AE_SCRIPT_API std::int32_t aether_editorgui_combo(const char* label, std::int32_t* current, const char* itemsNewlineJoined)
{
	// ImGui::Combo wants '\0'-separated + double-'\0'-terminated items.
	std::string items(itemsNewlineJoined != nullptr ? itemsNewlineJoined : "");
	for (char& c : items)
	{
		if (c == '\n')
		{
			c = '\0';
		}
	}
	items.push_back('\0');
	int cur = current != nullptr ? *current : 0;
	const bool changed = ImGui::Combo(label, &cur, items.c_str());
	if (current != nullptr)
	{
		*current = cur;
	}
	return changed ? 1 : 0;
}

// ── Canvas draw list ────────────────────────────────────────────────────────
AE_SCRIPT_API void aether_editorgui_add_line(Vec2 a, Vec2 b, Vec4 col, float thick) { ImGui::GetWindowDrawList()->AddLine(V2(a), V2(b), U32(col), thick); }
AE_SCRIPT_API void aether_editorgui_add_rect_filled(Vec2 mn, Vec2 mx, Vec4 col, float rounding) { ImGui::GetWindowDrawList()->AddRectFilled(V2(mn), V2(mx), U32(col), rounding); }
AE_SCRIPT_API void aether_editorgui_add_rect(Vec2 mn, Vec2 mx, Vec4 col, float rounding, float thick) { ImGui::GetWindowDrawList()->AddRect(V2(mn), V2(mx), U32(col), rounding, 0, thick); }
AE_SCRIPT_API void aether_editorgui_add_bezier(Vec2 p1, Vec2 p2, Vec2 p3, Vec2 p4, Vec4 col, float thick) { ImGui::GetWindowDrawList()->AddBezierCubic(V2(p1), V2(p2), V2(p3), V2(p4), U32(col), thick); }
AE_SCRIPT_API void aether_editorgui_add_circle_filled(Vec2 c, float r, Vec4 col) { ImGui::GetWindowDrawList()->AddCircleFilled(V2(c), r, U32(col)); }
AE_SCRIPT_API void aether_editorgui_add_triangle_filled(Vec2 a, Vec2 b, Vec2 c, Vec4 col) { ImGui::GetWindowDrawList()->AddTriangleFilled(V2(a), V2(b), V2(c), U32(col)); }
AE_SCRIPT_API void aether_editorgui_add_text(Vec2 p, Vec4 col, const char* s) { ImGui::GetWindowDrawList()->AddText(V2(p), U32(col), s); }

// Semantic theme colour (0..10; see the ThemeColor map + EditorColor enum). Follows the live theme.
AE_SCRIPT_API Vec4 aether_editorgui_theme_color(std::int32_t id) { const ImVec4 c = ThemeColor(id); return {c.x, c.y, c.z, c.w}; }

// ── Interaction ─────────────────────────────────────────────────────────────
AE_SCRIPT_API std::int32_t aether_editorgui_invisible_button(const char* id, Vec2 size) { return ImGui::InvisibleButton(id, V2(size)) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_active() { return ImGui::IsItemActive() ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_hovered() { return ImGui::IsItemHovered() ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_clicked() { return ImGui::IsItemClicked() ? 1 : 0; }
AE_SCRIPT_API Vec2 aether_editorgui_mouse_pos()
{
	const ImVec2 p = ImGui::GetIO().MousePos;
	return {p.x, p.y};
}
AE_SCRIPT_API Vec2 aether_editorgui_mouse_drag_delta()
{
	const ImVec2 d = ImGui::GetMouseDragDelta();
	return {d.x, d.y};
}
AE_SCRIPT_API std::int32_t aether_editorgui_is_mouse_dragging() { return ImGui::IsMouseDragging(ImGuiMouseButton_Left) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_mouse_clicked() { return ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_mouse_down() { return ImGui::IsMouseDown(ImGuiMouseButton_Left) ? 1 : 0; }
AE_SCRIPT_API float aether_editorgui_mouse_wheel() { return ImGui::GetIO().MouseWheel; }
