using System.Numerics;

namespace AetherCore;

/// <summary>
/// Transient 2D lights and shadow casters, submitted per frame with no entity behind them - for
/// effects that are data-driven and short-lived: a drawn ink stroke, a projectile trail, a spell.
///
/// Submissions last exactly one frame. Re-submit every frame the effect should light the scene; stop
/// submitting and it simply stops - there is no lifetime to manage and nothing to clean up. For
/// anything persistent and authored, put a Point Light component on an entity instead.
///
/// These feed the same 2D light map as component lights, so a submitted light is shadowed by the
/// world, and a submitted occluder shadows every shadow-casting light.
/// </summary>
public static class Lighting2D
{
    /// <summary>Add a light for this frame at a world position. <paramref name="radius"/> is its reach
    /// in world units and <paramref name="intensity"/> its brightness at the centre (2-3 reads well in
    /// a dark scene). Set <paramref name="castsShadow"/> only when the light should ray-march shadows;
    /// leave it false for cheap fill.</summary>
    public static void SubmitLight(Vector2 position, float radius, Vector3 color, float intensity, bool castsShadow = false)
        => Native.aether_light2d_submit_light(position.X, position.Y, radius, color, intensity, castsShadow ? 1 : 0);

    /// <summary>Add a shadow caster for this frame: the segment <paramref name="a"/>-<paramref name="b"/>
    /// thickened by <paramref name="radius"/>. Shadow-only - it blocks light but draws nothing, so pair
    /// it with however the effect renders itself.</summary>
    public static void SubmitOccluder(Vector2 a, Vector2 b, float radius)
        => Native.aether_light2d_submit_occluder_capsule(a.X, a.Y, b.X, b.Y, radius);
}
