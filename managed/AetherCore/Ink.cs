using System.Numerics;

namespace AetherCore;

/// <summary>
/// Conjured-ink field: a single fullscreen SDF pass that unions every live ink capsule into one
/// continuous wet-ink layer (dark body, glowing rim, organic edges). Rebuild it each frame -
/// <see cref="Clear"/> once, then <see cref="AddSegment"/> for every visible stroke segment. The
/// look is entirely in the shader; scripts only supply geometry. Physics stays separate (capsule
/// colliders), so this is purely the visual layer.
/// </summary>
public static class Ink
{
    /// <summary>Drop all segments. Call once at the start of the frame before re-adding.</summary>
    public static void Clear() => Native.aether_ink_clear();

    /// <summary>Body (fill) and rim (glow) colours shared by all solid ink.</summary>
    public static void SetColors(Vector4 body, Vector4 rim) => Native.aether_ink_set_colors(body, rim);

    /// <summary>Add one ink capsule from <paramref name="a"/> to <paramref name="b"/> (world units).
    /// <paramref name="width"/> = half-thickness, <paramref name="alpha"/> 0..1 (age fade),
    /// <paramref name="glow"/> = rim intensity, <paramref name="ghost"/> = 1 for unanchored red ink.</summary>
    public static void AddSegment(Vector2 a, Vector2 b, float width, float alpha, float glow, float ghost)
        => Native.aether_ink_add_segment(a.X, a.Y, b.X, b.Y, width, alpha, glow, ghost);
}
