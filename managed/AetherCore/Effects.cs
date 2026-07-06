using System.Numerics;

namespace AetherCore;

/// <summary>Per-entity visual effects (plasma, molten, etc.) and their parameters.</summary>
public static class Effects
{
    /// <summary>Apply a named effect to an entity.</summary>
    public static void Set(Entity entity, string effectName) => Native.aether_effect_set(entity.Id, effectName);

    public static void SetColor(Entity entity, Vector3 rgb) => Native.aether_effect_set_color(entity.Id, rgb);

    public static void SetSpeed(Entity entity, float speed) => Native.aether_effect_set_speed(entity.Id, speed);

    public static void SetScale(Entity entity, float scale) => Native.aether_effect_set_scale(entity.Id, scale);

    public static void SetIntensity(Entity entity, float intensity) => Native.aether_effect_set_intensity(entity.Id, intensity);
}
