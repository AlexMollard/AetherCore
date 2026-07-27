using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>Horizontal text alignment.</summary>
public enum UiHAlign { Left = 0, Center = 1, Right = 2 }

/// <summary>Vertical text alignment.</summary>
public enum UiVAlign { Top = 0, Middle = 1, Bottom = 2 }

/// <summary>Characters a text box accepts. ANDed with its allowed-characters string.</summary>
public enum UiContentType { Any = 0, Integer = 1, Decimal = 2, Alphanumeric = 3, Host = 4 }

/// <summary>
/// In-game UI (Canvas) control from scripts. UI is built as entities - a Canvas
/// with Image / Text children - laid out with anchors, offsets and a pivot (the
/// same RectTransform model Unity uses). The engine reads UI live each frame, so
/// anything set here shows on the next rendered frame.
/// </summary>
public static class Ui
{
    // ── Creation ────────────────────────────────────────────────────────────────

    /// <summary>Create a full-screen UI canvas entity.</summary>
    public static Entity CreateCanvas() => new(Native.aether_ui_create_canvas());

    /// <summary>Create an image (panel) under <paramref name="canvas"/>. Pass an
    /// invalid entity to attach to the first canvas (creating one if none exists).</summary>
    public static Entity CreateImage(Entity canvas = default) => new(Native.aether_ui_create_image(canvas.Id));

    /// <summary>Create a text element under <paramref name="canvas"/> with initial text.</summary>
    public static Entity CreateText(Entity canvas = default, string text = "")
    {
        Entity e = new(Native.aether_ui_create_text(canvas.Id));
        if (!string.IsNullOrEmpty(text))
        {
            SetText(e, text);
        }
        return e;
    }

    // ── Text content ────────────────────────────────────────────────────────────

    /// <summary>Set a UI Text element's string. No-op if the entity has no UI Text.</summary>
    public static void SetText(Entity entity, string text) => Native.aether_ui_set_text(entity.Id, text);

    /// <summary>Read a UI Text element's current string (empty if none).</summary>
    public static unsafe string GetText(Entity entity)
    {
        Span<byte> buffer = stackalloc byte[512];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_ui_get_text(entity.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    // ── Layout (RectTransform) ────────────────────────────────────────────────────

    /// <summary>Anchor corners in normalized (0..1) parent space; equal min/max = a point anchor.</summary>
    public static void SetAnchors(Entity e, Vector2 min, Vector2 max) => Native.aether_ui_set_anchors(e.Id, min, max);

    /// <summary>Raw pixel offsets of the rect's corners from the anchors.</summary>
    public static void SetOffsets(Entity e, Vector2 min, Vector2 max) => Native.aether_ui_set_offsets(e.Id, min, max);

    /// <summary>The rect's pivot / origin in normalized (0..1) self space.</summary>
    public static void SetPivot(Entity e, Vector2 pivot) => Native.aether_ui_set_pivot(e.Id, pivot);

    /// <summary>Place a <paramref name="width"/> x <paramref name="height"/> box at
    /// anchored position (<paramref name="x"/>, <paramref name="y"/>), honouring the pivot.</summary>
    public static void SetRect(Entity e, float x, float y, float width, float height)
        => Native.aether_ui_set_rect(e.Id, x, y, width, height);

    // ── Text style ────────────────────────────────────────────────────────────────

    public static void SetTextColor(Entity e, Vector4 color) => Native.aether_ui_set_text_color(e.Id, color);
    public static void SetFontSize(Entity e, float pixelSize) => Native.aether_ui_set_font_size(e.Id, pixelSize);

    /// <summary>Set the text element's font by baked name (e.g. "IBMPlexMono-Italic", "PixelStorm").</summary>
    public static void SetFont(Entity e, string fontName) => Native.aether_ui_set_font(e.Id, fontName);

    /// <summary>Toggle word-wrap to the element's rect width (default on). Off = single line, for
    /// manually laid-out runs.</summary>
    public static void SetTextWrap(Entity e, bool wrap) => Native.aether_ui_set_text_wrap(e.Id, wrap ? 1 : 0);
    public static void SetTextAlign(Entity e, UiHAlign h, UiVAlign v) => Native.aether_ui_set_text_align(e.Id, (int)h, (int)v);

    // ── Image style ───────────────────────────────────────────────────────────────

    public static void SetImageColor(Entity e, Vector4 color) => Native.aether_ui_set_image_color(e.Id, color);
    public static void SetImageCornerRadius(Entity e, float radius) => Native.aether_ui_set_image_corner_radius(e.Id, radius);

    /// <summary>Show a texture (VFS path, e.g. "project://assets/icon.png") on a UI image;
    /// pass an empty string to clear back to a solid colour fill.</summary>
    public static void SetImageTexture(Entity e, string path) => Native.aether_ui_set_image_texture(e.Id, path);

    // ── Interactivity (poll these to make a button) ───────────────────────────────

    /// <summary>The element's resolved rect as (x, y, width, height) in screen px
    /// (valid after the first frame's layout pass).</summary>
    public static Vector4 GetRect(Entity e) => Native.aether_ui_get_rect(e.Id);

    /// <summary>True if <paramref name="point"/> (screen px) is inside the element's current rect.</summary>
    public static bool Contains(Entity e, Vector2 point) => Native.aether_ui_contains_point(e.Id, point) != 0;

    /// <summary>True while the cursor is over the element.</summary>
    public static bool IsHovered(Entity e) => Contains(e, Input.MousePosition);

    /// <summary>True while the element is hovered and the left mouse button is held.</summary>
    public static bool IsPressed(Entity e) => IsHovered(e) && Input.IsMouseDown(MouseButton.Left);

    /// <summary>True on the frame the element is clicked (hovered + left button pressed this frame).</summary>
    public static bool WasClicked(Entity e) => IsHovered(e) && Input.IsMousePressed(MouseButton.Left);

    // ── Selection / focus (driven by the engine's UiNavigationSystem) ─────────────
    // Give an element a UI Selectable component and the engine handles spatial keyboard
    // navigation + mouse hover/click across the active selectables. Screens just style by
    // IsFocused and act on WasActivated - no manual index/hover/click tracking.

    /// <summary>True while this selectable is the focused one.</summary>
    public static bool IsFocused(Entity e) => Native.aether_ui_is_focused(e.Id) != 0;

    /// <summary>True on the frame this selectable is activated (Enter/Space while focused, or a click).</summary>
    public static bool WasActivated(Entity e) => Native.aether_ui_was_activated(e.Id) != 0;

    /// <summary>Make this selectable the focused one.</summary>
    public static void SetFocus(Entity e) => Native.aether_ui_set_focus(e.Id);

    /// <summary>Enable/disable navigation to this selectable (locked items set false).</summary>
    public static void SetInteractable(Entity e, bool interactable) => Native.aether_ui_set_interactable(e.Id, interactable ? 1 : 0);

    // ── Widgets (engine-drawn UISlider / UIToggle / UIButton / UIProgressBar) ──────
    // These components render themselves and are driven by the engine's UiWidgetSystem
    // (keyboard adjust when focused, mouse drag, toggle flip). Scripts just read/seed the
    // value and poll WasChanged to persist.

    /// <summary>Current slider value in its own units (min..max).</summary>
    public static float GetSliderValue(Entity e) => Native.aether_ui_get_slider_value(e.Id);

    /// <summary>Set a slider's value (clamped to its min..max).</summary>
    public static void SetSliderValue(Entity e, float value) => Native.aether_ui_set_slider_value(e.Id, value);

    /// <summary>Current toggle state.</summary>
    public static bool GetToggle(Entity e) => Native.aether_ui_get_toggle(e.Id) != 0;

    /// <summary>Set a toggle's state.</summary>
    public static void SetToggle(Entity e, bool on) => Native.aether_ui_set_toggle(e.Id, on ? 1 : 0);

    /// <summary>Current progress-bar fill (0..1).</summary>
    public static float GetProgress(Entity e) => Native.aether_ui_get_progress(e.Id);

    /// <summary>Set a progress-bar's fill (clamped 0..1).</summary>
    public static void SetProgress(Entity e, float value) => Native.aether_ui_set_progress(e.Id, value);

    /// <summary>A button's label text.</summary>
    public static unsafe string GetButtonLabel(Entity e)
    {
        Span<byte> buffer = stackalloc byte[256];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_ui_get_button_label(e.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>Set a button's label (ASCII only).</summary>
    public static void SetButtonLabel(Entity e, string label) => Native.aether_ui_set_button_label(e.Id, label);

    /// <summary>True on the frame a slider or toggle on this entity was changed by the user
    /// (keyboard/drag/activation). Poll this to persist settings.</summary>
    public static bool WasChanged(Entity e) => Native.aether_ui_was_changed(e.Id) != 0;

    // ── Text box ──────────────────────────────────────────────────────────────────
    // A single-line editable field. Editing is modal: the player clicks it (or presses
    // Enter while it is focused) to start, and Enter/Escape/Tab/clicking away ends it.
    // Poll WasSubmitted to act on a committed value.

    /// <summary>Create an editable text box under <paramref name="canvas"/>.</summary>
    public static Entity CreateTextBox(Entity canvas = default) => new(Native.aether_ui_create_text_box(canvas.Id));

    /// <summary>The text box's current string (empty if the entity has none).</summary>
    public static unsafe string GetTextBoxText(Entity e)
    {
        Span<byte> buffer = stackalloc byte[512];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_ui_get_text_box_text(e.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>Replace the text box's string; the caret moves to the end (ASCII only).</summary>
    public static void SetTextBoxText(Entity e, string text) => Native.aether_ui_set_text_box_text(e.Id, text);

    /// <summary>The greyed-out hint shown while the box is empty.</summary>
    public static void SetPlaceholder(Entity e, string text) => Native.aether_ui_set_text_box_placeholder(e.Id, text);

    /// <summary>Restrict which characters the box accepts.</summary>
    public static void SetContentType(Entity e, UiContentType type) => Native.aether_ui_set_text_box_content_type(e.Id, (int)type);

    /// <summary>True on the frame the player pressed Enter to commit the field.</summary>
    public static bool WasSubmitted(Entity e) => Native.aether_ui_was_submitted(e.Id) != 0;

    /// <summary>True on the frame the player pressed Escape, reverting to the value on entry.</summary>
    public static bool WasCancelled(Entity e) => Native.aether_ui_was_cancelled(e.Id) != 0;

    /// <summary>True while the box owns the keyboard.</summary>
    public static bool IsEditing(Entity e) => Native.aether_ui_is_editing(e.Id) != 0;

    /// <summary>Focus the box and start editing, as a click would.</summary>
    public static void BeginEdit(Entity e) => Native.aether_ui_begin_edit(e.Id);

    // ── Custom-shader effects ─────────────────────────────────────────────────────
    // A UI element rendered by its own shader ("shaders://&lt;shader&gt;.spv"), drawn on top of the
    // batched UI. The shader reads params + two colours as push constants; meaning is shader-defined
    // (e.g. "ui_ink": params = (coverage, time, noiseAmp, edgeWidth), color0 = ink, color1 = edge).

    /// <summary>Create a full-screen effect element driven by the named UI shader.</summary>
    public static Entity CreateEffect(Entity canvas, string shader) => new(Native.aether_ui_create_effect(canvas.Id, shader));

    /// <summary>Set the effect's shader params (meaning is shader-defined).</summary>
    public static void SetEffectParams(Entity e, Vector4 param) => Native.aether_ui_set_effect_params(e.Id, param);

    /// <summary>Set the effect's two shader colours.</summary>
    public static void SetEffectColors(Entity e, Vector4 color0, Vector4 color1) => Native.aether_ui_set_effect_colors(e.Id, color0, color1);

    /// <summary>Set the effect's draw order within its background/overlay group: higher = drawn later
    /// (on top). Use a high value for a screen-transition overlay so it sits above per-screen overlays.</summary>
    public static void SetEffectSortOrder(Entity e, int sortOrder) => Native.aether_ui_set_effect_sort_order(e.Id, sortOrder);

    // ── Per-element materials ────────────────────────────────────────────────────
    // A UIMaterial applies a custom fragment shader to the element's OWN draw commands (its text
    // glyphs / image / rect), masked to their shapes - not a separate quad. params.x is conventionally
    // time. Add it via the editor/scene or SetMaterial, then animate with SetMaterialParams.

    /// <summary>Apply (get-or-add) a custom fragment shader to this UI element's own draw.</summary>
    public static void SetMaterial(Entity e, string shader) => Native.aether_ui_set_material(e.Id, shader);

    /// <summary>Set the element material's shader params (meaning is shader-defined; x is usually time).</summary>
    public static void SetMaterialParams(Entity e, Vector4 param) => Native.aether_ui_set_material_params(e.Id, param);

    /// <summary>Set the element material's two shader colours.</summary>
    public static void SetMaterialColors(Entity e, Vector4 color0, Vector4 color1) => Native.aether_ui_set_material_colors(e.Id, color0, color1);
}
