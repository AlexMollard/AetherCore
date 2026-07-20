using System.Numerics;

namespace AetherCore;

public enum SpriteBlendMode
{
    Alpha = 0,
    Additive,
    Multiply,
    Opaque,
}

public static class SpriteRenderer
{
    public static void SetTexture(Entity entity, string path) => Native.aether_sprite_set_texture(entity.Id, path);
    public static Vector4 GetTint(Entity entity) => Native.aether_sprite_get_tint(entity.Id);
    public static void SetTint(Entity entity, Vector4 value) => Native.aether_sprite_set_tint(entity.Id, value);
    public static Vector2 GetPixelSize(Entity entity) => Native.aether_sprite_get_pixel_size(entity.Id);
    public static void SetPixelSize(Entity entity, Vector2 value) => Native.aether_sprite_set_pixel_size(entity.Id, value);
    public static Vector2 GetPivot(Entity entity) => Native.aether_sprite_get_pivot(entity.Id);
    public static void SetPivot(Entity entity, Vector2 value) => Native.aether_sprite_set_pivot(entity.Id, value);
    public static float GetPixelsPerUnit(Entity entity) => Native.aether_sprite_get_pixels_per_unit(entity.Id);
    public static void SetPixelsPerUnit(Entity entity, float value) => Native.aether_sprite_set_pixels_per_unit(entity.Id, value);
    public static int GetSortingLayer(Entity entity) => Native.aether_sprite_get_sorting_layer(entity.Id);
    public static void SetSortingLayer(Entity entity, int value) => Native.aether_sprite_set_sorting_layer(entity.Id, value);
    public static int GetOrderInLayer(Entity entity) => Native.aether_sprite_get_order_in_layer(entity.Id);
    public static void SetOrderInLayer(Entity entity, int value) => Native.aether_sprite_set_order_in_layer(entity.Id, value);
    public static SpriteBlendMode GetBlendMode(Entity entity) => (SpriteBlendMode)Native.aether_sprite_get_blend_mode(entity.Id);
    public static void SetBlendMode(Entity entity, SpriteBlendMode value) => Native.aether_sprite_set_blend_mode(entity.Id, (int)value);

    /// <summary>Mirror the sprite horizontally (the way to face movement - negative
    /// transform scale does not survive the 2D physics transform sync).</summary>
    public static bool GetFlipX(Entity entity) => (GetFlags(entity) & 2u) != 0u;
    public static void SetFlipX(Entity entity, bool value) => SetFlag(entity, 2u, value);
    public static bool GetFlipY(Entity entity) => (GetFlags(entity) & 4u) != 0u;
    public static void SetFlipY(Entity entity, bool value) => SetFlag(entity, 4u, value);
    public static bool GetVisible(Entity entity) => (GetFlags(entity) & 1u) != 0u;
    public static void SetVisible(Entity entity, bool value) => SetFlag(entity, 1u, value);

    private static void SetFlag(Entity entity, uint bit, bool value)
    {
        uint flags = GetFlags(entity);
        SetFlags(entity, value ? flags | bit : flags & ~bit);
    }

    internal static uint GetFlags(Entity entity) => Native.aether_sprite_get_flags(entity.Id);
    internal static void SetFlags(Entity entity, uint flags) => Native.aether_sprite_set_flags(entity.Id, flags);
}
