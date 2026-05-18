#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#ifdef AETHER_IMGUI
#	include <imgui.h>
#endif

// ── Binding functions ─────────────────────────────────────────────────────────
// All ImGui calls are guarded so the module compiles when AETHER_IMGUI is off.
// Scripts can always `require imgui`; functions are simply no-ops in that case.

namespace
{
	// ── Windows ───────────────────────────────────────────────────────────────

	bool das_imgui_begin(const char* title)
	{
#ifdef AETHER_IMGUI
		return ImGui::Begin(title);
#else
		return false;
#endif
	}

	// begin_closable returns whether the window is open; `open` is set to false
	// when the user clicks the [x] close button.
	bool das_imgui_begin_closable(const char* title, bool& open)
	{
#ifdef AETHER_IMGUI
		return ImGui::Begin(title, &open);
#else
		return false;
#endif
	}

	void das_imgui_end()
	{
#ifdef AETHER_IMGUI
		ImGui::End();
#endif
	}

	void das_imgui_set_next_window_pos(float x, float y)
	{
#ifdef AETHER_IMGUI
		ImGui::SetNextWindowPos({ x, y });
#endif
	}

	// Sets position only on first use - allows user to drag the window afterward.
	void das_imgui_set_next_window_pos_once(float x, float y)
	{
#ifdef AETHER_IMGUI
		ImGui::SetNextWindowPos({ x, y }, ImGuiCond_FirstUseEver);
#endif
	}

	void das_imgui_set_next_window_size(float w, float h)
	{
#ifdef AETHER_IMGUI
		ImGui::SetNextWindowSize({ w, h });
#endif
	}

	// Sets size only on first use - allows user to resize the window afterward.
	void das_imgui_set_next_window_size_once(float w, float h)
	{
#ifdef AETHER_IMGUI
		ImGui::SetNextWindowSize({ w, h }, ImGuiCond_FirstUseEver);
#endif
	}

	// ── Text ──────────────────────────────────────────────────────────────────

	void das_imgui_text(const char* text)
	{
#ifdef AETHER_IMGUI
		ImGui::TextUnformatted(text);
#endif
	}

	void das_imgui_text_colored(das::float4 color, const char* text)
	{
#ifdef AETHER_IMGUI
		ImGui::TextColored({ color.x, color.y, color.z, color.w }, "%s", text);
#endif
	}

	void das_imgui_label_text(const char* label, const char* value)
	{
#ifdef AETHER_IMGUI
		ImGui::LabelText(label, "%s", value);
#endif
	}

	// ── Widgets ───────────────────────────────────────────────────────────────

	bool das_imgui_button(const char* label)
	{
#ifdef AETHER_IMGUI
		return ImGui::Button(label);
#else
		return false;
#endif
	}

	bool das_imgui_button_sized(const char* label, float w, float h)
	{
#ifdef AETHER_IMGUI
		return ImGui::Button(label, { w, h });
#else
		return false;
#endif
	}

	// Checkbox - `checked` is passed by reference; daScript callers write:
	//   var my_flag = true
	//   imgui_checkbox("Enable", my_flag)
	bool das_imgui_checkbox(const char* label, bool& checked)
	{
#ifdef AETHER_IMGUI
		return ImGui::Checkbox(label, &checked);
#else
		return false;
#endif
	}

	// Sliders - value passed by reference so the script variable is updated.
	bool das_imgui_slider_float(const char* label, float& value, float min, float max)
	{
#ifdef AETHER_IMGUI
		return ImGui::SliderFloat(label, &value, min, max);
#else
		return false;
#endif
	}

	bool das_imgui_slider_int(const char* label, int& value, int min, int max)
	{
#ifdef AETHER_IMGUI
		return ImGui::SliderInt(label, &value, min, max);
#else
		return false;
#endif
	}

	bool das_imgui_input_float(const char* label, float& value)
	{
#ifdef AETHER_IMGUI
		return ImGui::InputFloat(label, &value);
#else
		return false;
#endif
	}

	bool das_imgui_input_int(const char* label, int& value)
	{
#ifdef AETHER_IMGUI
		return ImGui::InputInt(label, &value);
#else
		return false;
#endif
	}

	// drag_float - more compact than slider; useful for small values.
	bool das_imgui_drag_float(const char* label, float& value, float speed)
	{
#ifdef AETHER_IMGUI
		return ImGui::DragFloat(label, &value, speed);
#else
		return false;
#endif
	}

	bool das_imgui_drag_int(const char* label, int& value, float speed)
	{
#ifdef AETHER_IMGUI
		return ImGui::DragInt(label, &value, speed);
#else
		return false;
#endif
	}

	// color_edit4 - edits r, g, b, a individually; returns true if changed.
	bool das_imgui_color_edit4(const char* label, float& r, float& g, float& b, float& a)
	{
#ifdef AETHER_IMGUI
		float col[4] = { r, g, b, a };
		bool changed = ImGui::ColorEdit4(label, col);
		if (changed)
		{
			r = col[0];
			g = col[1];
			b = col[2];
			a = col[3];
		}
		return changed;
#else
		return false;
#endif
	}

	// color_edit3 - RGB only.
	bool das_imgui_color_edit3(const char* label, float& r, float& g, float& b)
	{
#ifdef AETHER_IMGUI
		float col[3] = { r, g, b };
		bool changed = ImGui::ColorEdit3(label, col);
		if (changed)
		{
			r = col[0];
			g = col[1];
			b = col[2];
		}
		return changed;
#else
		return false;
#endif
	}

	// ── Trees / collapsibles ──────────────────────────────────────────────────

	bool das_imgui_collapsing_header(const char* label)
	{
#ifdef AETHER_IMGUI
		return ImGui::CollapsingHeader(label);
#else
		return false;
#endif
	}

	bool das_imgui_tree_node(const char* label)
	{
#ifdef AETHER_IMGUI
		return ImGui::TreeNode(label);
#else
		return false;
#endif
	}

	void das_imgui_tree_pop()
	{
#ifdef AETHER_IMGUI
		ImGui::TreePop();
#endif
	}

	// ── Layout helpers ────────────────────────────────────────────────────────

	void das_imgui_separator()
	{
#ifdef AETHER_IMGUI
		ImGui::Separator();
#endif
	}

	void das_imgui_same_line()
	{
#ifdef AETHER_IMGUI
		ImGui::SameLine();
#endif
	}

	void das_imgui_spacing()
	{
#ifdef AETHER_IMGUI
		ImGui::Spacing();
#endif
	}

	void das_imgui_new_line()
	{
#ifdef AETHER_IMGUI
		ImGui::NewLine();
#endif
	}

	void das_imgui_indent(float w)
	{
#ifdef AETHER_IMGUI
		ImGui::Indent(w);
#endif
	}

	void das_imgui_unindent(float w)
	{
#ifdef AETHER_IMGUI
		ImGui::Unindent(w);
#endif
	}

	// ── Query ─────────────────────────────────────────────────────────────────

	bool das_imgui_is_item_hovered()
	{
#ifdef AETHER_IMGUI
		return ImGui::IsItemHovered();
#else
		return false;
#endif
	}

	bool das_imgui_is_item_clicked()
	{
#ifdef AETHER_IMGUI
		return ImGui::IsItemClicked();
#else
		return false;
#endif
	}

	float das_imgui_get_frame_rate()
	{
#ifdef AETHER_IMGUI
		return ImGui::GetIO().Framerate;
#else
		return 0.f;
#endif
	}

} // namespace

// ── Module ────────────────────────────────────────────────────────────────────

namespace aether::app::scripting
{
	struct ImGuiModule : DasModuleBase
	{
		ImGuiModule()
		      : DasModuleBase("imgui")
		{
			das::ModuleLibrary lib(this);

			// Windows
			Bind<das_imgui_begin>(lib, "imgui_begin", SE::modifyExternal);
			Bind<das_imgui_begin_closable>(lib, "imgui_begin_closable", SE::modifyExternal);
			Bind<das_imgui_end>(lib, "imgui_end", SE::modifyExternal);
			Bind<das_imgui_set_next_window_pos>(lib, "imgui_set_next_window_pos", SE::modifyExternal);
			Bind<das_imgui_set_next_window_pos_once>(lib, "imgui_set_next_window_pos_once", SE::modifyExternal);
			Bind<das_imgui_set_next_window_size>(lib, "imgui_set_next_window_size", SE::modifyExternal);
			Bind<das_imgui_set_next_window_size_once>(lib, "imgui_set_next_window_size_once", SE::modifyExternal);

			// Text
			Bind<das_imgui_text>(lib, "imgui_text", SE::modifyExternal);
			Bind<das_imgui_text_colored>(lib, "imgui_text_colored", SE::modifyExternal);
			Bind<das_imgui_label_text>(lib, "imgui_label_text", SE::modifyExternal);

			// Widgets
			Bind<das_imgui_button>(lib, "imgui_button", SE::modifyExternal);
			Bind<das_imgui_button_sized>(lib, "imgui_button_sized", SE::modifyExternal);
			Bind<das_imgui_checkbox>(lib, "imgui_checkbox", SE::modifyExternal);
			Bind<das_imgui_slider_float>(lib, "imgui_slider_float", SE::modifyExternal);
			Bind<das_imgui_slider_int>(lib, "imgui_slider_int", SE::modifyExternal);
			Bind<das_imgui_input_float>(lib, "imgui_input_float", SE::modifyExternal);
			Bind<das_imgui_input_int>(lib, "imgui_input_int", SE::modifyExternal);
			Bind<das_imgui_drag_float>(lib, "imgui_drag_float", SE::modifyExternal);
			Bind<das_imgui_drag_int>(lib, "imgui_drag_int", SE::modifyExternal);
			Bind<das_imgui_color_edit4>(lib, "imgui_color_edit4", SE::modifyExternal);
			Bind<das_imgui_color_edit3>(lib, "imgui_color_edit3", SE::modifyExternal);

			// Trees
			Bind<das_imgui_collapsing_header>(lib, "imgui_collapsing_header", SE::modifyExternal);
			Bind<das_imgui_tree_node>(lib, "imgui_tree_node", SE::modifyExternal);
			Bind<das_imgui_tree_pop>(lib, "imgui_tree_pop", SE::modifyExternal);

			// Layout
			Bind<das_imgui_separator>(lib, "imgui_separator", SE::modifyExternal);
			Bind<das_imgui_same_line>(lib, "imgui_same_line", SE::modifyExternal);
			Bind<das_imgui_spacing>(lib, "imgui_spacing", SE::modifyExternal);
			Bind<das_imgui_new_line>(lib, "imgui_new_line", SE::modifyExternal);
			Bind<das_imgui_indent>(lib, "imgui_indent", SE::modifyExternal);
			Bind<das_imgui_unindent>(lib, "imgui_unindent", SE::modifyExternal);

			// Query
			Bind<das_imgui_is_item_hovered>(lib, "imgui_is_item_hovered", SE::accessExternal);
			Bind<das_imgui_is_item_clicked>(lib, "imgui_is_item_clicked", SE::accessExternal);
			Bind<das_imgui_get_frame_rate>(lib, "imgui_get_frame_rate", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(ImGuiModule, aether::app::scripting)
