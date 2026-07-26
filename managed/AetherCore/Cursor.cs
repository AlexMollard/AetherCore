using System.Numerics;

namespace AetherCore;

/// <summary>
/// The mouse pointer.
///
/// The engine draws it and the project configures it - turn it on and name its art under
/// <c>[cursor]</c> in ProjectSettings.toml and there is nothing to write. It needs no entity, survives
/// scene loads, and always composites over every canvas, so none of the usual pointer problems exist.
///
/// This API is for games that need MORE than the one configured pointer: a different look for a
/// different verb (<see cref="SetLook"/>), or no pointer at all for a stretch where the game draws its
/// own marker at the cursor and would otherwise draw two (<see cref="Visible"/>).
/// </summary>
public static class Cursor
{
    /// <summary>Whether the engine is drawing a pointer for this project at all. False unless
    /// <c>cursor.custom</c> is set in project settings; nothing here does anything when it is off.</summary>
    public static bool Enabled => Native.aether_cursor_enabled() != 0;

    /// <summary>Draw the pointer, or hide it without giving the job back to the OS. Hide it wherever
    /// the game already marks the cursor position itself.</summary>
    public static bool Visible
    {
        get => Native.aether_cursor_get_visible() != 0;
        set => Native.aether_cursor_set_visible(value ? 1 : 0);
    }

    /// <summary>Swap the pointer's art until <see cref="Reset"/>. The hotspot is the fraction across the
    /// image (0,0 = top-left, 1,1 = bottom-right) that sits on the mouse, so an arrow points from its
    /// corner and a pen writes from its tip without either knowing how the other is anchored.</summary>
    public static void SetLook(string texture, Vector2 hotspot, float size, bool pixelArt = true)
        => Native.aether_cursor_set_look(texture, hotspot.X, hotspot.Y, size, pixelArt ? 1 : 0);

    /// <summary>Back to whatever the project configured.</summary>
    public static void Reset() => Native.aether_cursor_reset();
}
