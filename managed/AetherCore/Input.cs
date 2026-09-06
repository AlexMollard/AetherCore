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

    /// <summary>The key that transitioned down THIS frame, or <see cref="Key.None"/> if
    /// none did - for a "press any key to rebind" prompt, instead of scanning every
    /// <see cref="Key"/> value with <see cref="IsKeyPressed"/> each frame. If two keys
    /// land on the same frame, the lower enum value wins - there is no principled way
    /// to prefer one over the other without knowing what the caller considers a "real"
    /// key versus a modifier.</summary>
    public static Key NextKeyPressed() => (Key)Native.aether_input_next_key_pressed();

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

    /// <summary>
    /// Whether the game currently wants FPS-style pointer lock: cursor hidden, confined to
    /// the window, with <see cref="MouseDelta"/> reporting unbounded relative motion instead
    /// of an absolute position that stops dead at the screen edge. Only takes effect while
    /// actually playing and the window is focused - see <see cref="IsCursorLocked"/> for
    /// whether it is actually in effect right now. Has no effect in the editor's edit mode,
    /// and is released automatically on losing focus, on Stop, or when the user presses
    /// Escape (the editor's escape hatch back to its own UI) - set it again to re-lock.
    /// </summary>
    public static bool CursorLockRequested
    {
        get => Native.aether_input_get_cursor_lock_requested() != 0;
        set => Native.aether_input_request_cursor_lock(value ? 1 : 0);
    }

    /// <summary>True while the pointer is actually locked right now (requested, focused, and not escaped).</summary>
    public static bool IsCursorLocked => Native.aether_input_is_cursor_locked() != 0;

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
