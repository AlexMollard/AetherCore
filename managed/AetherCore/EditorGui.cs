using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>Immediate-mode ImGui, callable from an <see cref="IEditorWindow"/>.OnGui(). Editor/dev-tooling
/// only - these forward to the editor's live ImGui context. In a shipped game the underlying exports are
/// absent, but nothing calls them, so they resolve lazily and are never hit.</summary>
/// <summary>Semantic slots mapped to the editor's live ImGui theme colours (see EditorGui.ThemeColor).
/// Use these for custom draw-list drawing so a project window follows whatever theme is active.</summary>
public enum EditorColor
{
    WindowBg = 0, ChildBg = 1, PanelBg = 2, PanelHover = 3, Header = 4,
    Text = 5, TextDim = 6, Border = 7, Accent = 8, AccentDim = 9, Link = 10,
}

public static unsafe class EditorGui
{
    /// <summary>A colour from the editor's current theme (follows the active ImGui style).</summary>
    public static Vector4 ThemeColor(EditorColor c) => Native.aether_editorgui_theme_color((int)c);

    // ── Windows / layout ──────────────────────────────────────────────────────
    /// <summary>Initial size the next window opens at; the user can still resize (persists).</summary>
    public static void SetNextWindowSize(Vector2 size) => Native.aether_editorgui_set_next_window_size(size);

    public static bool Begin(string title)
    {
        int open = 1;
        return Native.aether_editorgui_begin(title, &open) != 0;
    }

    public static bool Begin(string title, ref bool open)
    {
        int o = open ? 1 : 0;
        bool visible = Native.aether_editorgui_begin(title, &o) != 0;
        open = o != 0;
        return visible;
    }

    public static void End() => Native.aether_editorgui_end();

    public static bool BeginChild(string id, Vector2 size = default, bool border = false) => Native.aether_editorgui_begin_child(id, size, border ? 1 : 0) != 0;
    public static void EndChild() => Native.aether_editorgui_end_child();
    public static void SameLine() => Native.aether_editorgui_same_line();
    /// <summary>Width of the next framed widget (Combo/InputText/...); negative fills to the right edge.</summary>
    public static void SetNextItemWidth(float width) => Native.aether_editorgui_set_next_item_width(width);
    /// <summary>Height of a standard framed widget (font + frame padding) - for centering custom layout.</summary>
    public static float FrameHeight() => Native.aether_editorgui_frame_height();
    public static Vector2 CalcTextSize(string s) => Native.aether_editorgui_calc_text_size(s);
    /// <summary>Clip subsequent draw-list drawing to a rect (balance with PopClipRect).</summary>
    public static void PushClipRect(Vector2 min, Vector2 max, bool intersectCurrent = true) => Native.aether_editorgui_push_clip_rect(min, max, intersectCurrent ? 1 : 0);
    public static void PopClipRect() => Native.aether_editorgui_pop_clip_rect();
    public static void Separator() => Native.aether_editorgui_separator();
    public static void Spacing() => Native.aether_editorgui_spacing();
    public static Vector2 ContentAvail() => Native.aether_editorgui_content_avail();
    public static Vector2 CursorScreenPos() => Native.aether_editorgui_cursor_screen_pos();
    public static void SetCursorScreenPos(Vector2 p) => Native.aether_editorgui_set_cursor_screen_pos(p);

    // ── Text / widgets ────────────────────────────────────────────────────────
    public static void Text(string s) => Native.aether_editorgui_text(s);
    public static void TextColored(Vector4 color, string s) => Native.aether_editorgui_text_colored(color, s);
    public static bool Button(string label, Vector2 size = default) => Native.aether_editorgui_button(label, size) != 0;
    public static bool SmallButton(string label) => Native.aether_editorgui_small_button(label) != 0;

    public static bool Checkbox(string label, ref bool v)
    {
        int i = v ? 1 : 0;
        bool changed = Native.aether_editorgui_checkbox(label, &i) != 0;
        v = i != 0;
        return changed;
    }

    /// <summary>Editable text field. Returns true on the frame the text changed.</summary>
    public static bool InputText(string label, ref string text, int maxLen = 256)
    {
        if (maxLen < 2) { maxLen = 2; }
        byte[] buf = new byte[maxLen];
        int n = Encoding.UTF8.GetBytes(text ?? string.Empty, 0, Math.Min((text ?? string.Empty).Length, maxLen - 1), buf, 0);
        buf[n] = 0;
        bool changed;
        fixed (byte* p = buf)
        {
            changed = Native.aether_editorgui_input_text(label, p, buf.Length) != 0;
            if (changed)
            {
                int len = 0;
                while (len < buf.Length && p[len] != 0) { len++; }
                text = Encoding.UTF8.GetString(p, len);
            }
        }
        return changed;
    }

    /// <summary>Multi-line editable text field of the given size. Returns true on the frame it changed.</summary>
    public static bool InputTextMultiline(string label, ref string text, Vector2 size, int maxLen = 1024)
    {
        if (maxLen < 2) { maxLen = 2; }
        byte[] buf = new byte[maxLen];
        int n = Encoding.UTF8.GetBytes(text ?? string.Empty, 0, Math.Min((text ?? string.Empty).Length, maxLen - 1), buf, 0);
        buf[n] = 0;
        bool changed;
        fixed (byte* p = buf)
        {
            changed = Native.aether_editorgui_input_text_multiline(label, p, buf.Length, size) != 0;
            if (changed)
            {
                int len = 0;
                while (len < buf.Length && p[len] != 0) { len++; }
                text = Encoding.UTF8.GetString(p, len);
            }
        }
        return changed;
    }

    public static bool InputFloat(string label, ref float v)
    {
        fixed (float* p = &v) { return Native.aether_editorgui_input_float(label, p) != 0; }
    }

    public static bool Selectable(string label, bool selected = false) => Native.aether_editorgui_selectable(label, selected ? 1 : 0) != 0;
    public static bool TreeNode(string label) => Native.aether_editorgui_tree_node(label) != 0;
    public static void TreePop() => Native.aether_editorgui_tree_pop();

    public static bool Combo(string label, ref int current, string[] items)
    {
        int cur = current;
        bool changed = Native.aether_editorgui_combo(label, &cur, string.Join('\n', items)) != 0;
        current = cur;
        return changed;
    }

    // ── Canvas draw list ──────────────────────────────────────────────────────
    public static void AddLine(Vector2 a, Vector2 b, Vector4 color, float thickness = 1f) => Native.aether_editorgui_add_line(a, b, color, thickness);
    public static void AddRectFilled(Vector2 min, Vector2 max, Vector4 color, float rounding = 0f) => Native.aether_editorgui_add_rect_filled(min, max, color, rounding);
    public static void AddRect(Vector2 min, Vector2 max, Vector4 color, float rounding = 0f, float thickness = 1f) => Native.aether_editorgui_add_rect(min, max, color, rounding, thickness);
    public static void AddBezierCubic(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4, Vector4 color, float thickness = 1f) => Native.aether_editorgui_add_bezier(p1, p2, p3, p4, color, thickness);
    public static void AddCircleFilled(Vector2 center, float radius, Vector4 color) => Native.aether_editorgui_add_circle_filled(center, radius, color);
    public static void AddTriangleFilled(Vector2 a, Vector2 b, Vector2 c, Vector4 color) => Native.aether_editorgui_add_triangle_filled(a, b, c, color);
    public static void AddText(Vector2 pos, Vector4 color, string s) => Native.aether_editorgui_add_text(pos, color, s);

    // ── Interaction ───────────────────────────────────────────────────────────
    public static bool InvisibleButton(string id, Vector2 size) => Native.aether_editorgui_invisible_button(id, size) != 0;
    public static bool IsItemActive() => Native.aether_editorgui_is_item_active() != 0;
    public static bool IsItemHovered() => Native.aether_editorgui_is_item_hovered() != 0;
    public static bool IsItemClicked() => Native.aether_editorgui_is_item_clicked() != 0;
    public static Vector2 MousePos() => Native.aether_editorgui_mouse_pos();
    public static Vector2 MouseDragDelta() => Native.aether_editorgui_mouse_drag_delta();
    public static bool IsMouseDragging() => Native.aether_editorgui_is_mouse_dragging() != 0;
    public static bool IsMouseClicked() => Native.aether_editorgui_is_mouse_clicked() != 0;
    public static bool IsMouseDown() => Native.aether_editorgui_is_mouse_down() != 0;
    public static float MouseWheel() => Native.aether_editorgui_mouse_wheel();
}
