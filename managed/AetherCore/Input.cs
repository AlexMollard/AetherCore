using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>Mouse buttons (values match the engine's MouseButton).</summary>
public enum MouseButton { Left = 0, Right = 1, Middle = 2 }

/// <summary>Keyboard and mouse state for the current frame.</summary>
public static class Input
{
    // ── Keyboard ────────────────────────────────────────────────────────────────
    public static bool IsKeyDown(Key key) => Native.aether_input_key_down((int)key) != 0;
    public static bool IsKeyPressed(Key key) => Native.aether_input_key_pressed((int)key) != 0;
    public static bool IsKeyReleased(Key key) => Native.aether_input_key_released((int)key) != 0;

    /// <summary>Seconds elapsed since the previous frame.</summary>
    public static float DeltaTime => Native.aether_input_delta_time();

    // ── Mouse ─────────────────────────────────────────────────────────────────────

    /// <summary>Cursor position in render-target pixels (top-left origin). Hit-tests UI directly.</summary>
    public static Vector2 MousePosition => Native.aether_input_mouse_pos();

    /// <summary>Cursor movement since the previous frame, in pixels.</summary>
    public static Vector2 MouseDelta => Native.aether_input_mouse_delta();

    /// <summary>Scroll-wheel movement since the previous frame (y = vertical).</summary>
    public static Vector2 ScrollDelta => Native.aether_input_scroll_delta();

    /// <summary>Whether the operating system draws its own pointer. Turn it off to draw your own -
    /// the pointer keeps its real screen position and keeps reporting normally, it is just not
    /// painted, so nothing about input, window chrome or alt-tab changes.</summary>
    public static bool OsCursorVisible
    {
        get => Native.aether_input_get_os_cursor_visible() != 0;
        set => Native.aether_input_set_os_cursor_visible(value ? 1 : 0);
    }

    public static bool IsMouseDown(MouseButton button) => Native.aether_input_mouse_down((int)button) != 0;
    public static bool IsMousePressed(MouseButton button) => Native.aether_input_mouse_pressed((int)button) != 0;
    public static bool IsMouseReleased(MouseButton button) => Native.aether_input_mouse_released((int)button) != 0;

    // ── Axes ──────────────────────────────────────────────────────────────────────

    /// <summary>Raw -1 / 0 / +1 axis from two keys (e.g. A / D). No smoothing.</summary>
    public static float GetAxisRaw(Key negative, Key positive)
    {
        float v = 0.0f;
        if (IsKeyDown(negative)) { v -= 1.0f; }
        if (IsKeyDown(positive)) { v += 1.0f; }
        return v;
    }

    // ── Clipboard ─────────────────────────────────────────────────────────────────

    /// <summary>The OS clipboard's text contents.</summary>
    public static unsafe string Clipboard
    {
        get
        {
            Span<byte> buffer = stackalloc byte[1024];
            fixed (byte* ptr = buffer)
            {
                int written = Native.aether_input_get_clipboard(ptr, buffer.Length);
                return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
            }
        }
        set => Native.aether_input_set_clipboard(value ?? string.Empty);
    }
}
