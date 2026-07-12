using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>Horizontal text alignment.</summary>
public enum UiHAlign { Left = 0, Center = 1, Right = 2 }

/// <summary>Vertical text alignment.</summary>
public enum UiVAlign { Top = 0, Middle = 1, Bottom = 2 }

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
}
