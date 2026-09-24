using System.Numerics;

namespace AetherCore;

/// <summary>Handle to a shared, authored material (0 is invalid).</summary>
public readonly record struct MaterialId(uint Value)
{
    public bool IsValid => Value != 0xFFFFFFFF && Value != 0;

    public void SetColor(Vector3 color) => Native.aether_material_set_color(Value, color);

    public void SetMetallic(float value) => Native.aether_material_set_metallic(Value, value);

    public void SetRoughness(float value) => Native.aether_material_set_roughness(Value, value);

    public void SetEmissive(Vector3 color) => Native.aether_material_set_emissive(Value, color);
}

/// <summary>A cached primitive mesh handle from <see cref="World.CreateMesh"/>.</summary>
public readonly record struct MeshHandle(uint Value);

/// <summary>
/// Per-entity copy-on-write material edits. Obtained via <see cref="Entity.Material"/>;
/// each setter mutates only this entity's material instance.
/// </summary>
public readonly struct MaterialEditor
{
    private readonly uint _entityId;

    internal MaterialEditor(uint entityId) => _entityId = entityId;

    public void SetColor(Vector3 color) => Native.aether_entity_material_set_color(_entityId, color);

    public void SetMetallic(float value) => Native.aether_entity_material_set_metallic(_entityId, value);

    public void SetRoughness(float value) => Native.aether_entity_material_set_roughness(_entityId, value);

    public void SetEmissive(Vector3 color) => Native.aether_entity_material_set_emissive(_entityId, color);

    /// <summary>The entity's current emissive value - whatever a per-entity override
    /// last set, else whatever its shared material asset describes, else black. Reads,
    /// never mutates - see aether_entity_material_get_emissive's own native-side
    /// comment on why a plain query must not have the side effect a setter's
    /// seed-on-first-write does.</summary>
    public Vector3 GetEmissive() => Native.aether_entity_material_get_emissive(_entityId);
}

/// <summary>Factory for shared authored materials.</summary>
public static class Material
{
    /// <summary>Create a shared material; edit it, bind it to many entities.</summary>
    public static MaterialId Make(Vector3 color, float metallic, float roughness)
        => new(Native.aether_make_material(color, metallic, roughness));

    // ── Per-entity material (edits this entity's own material instance) ───────────

    /// <summary>Apply a texture (VFS path) as this entity's albedo map.</summary>
    public static void SetTexture(Entity entity, string path) => Native.aether_set_material_texture(entity.Id, path);

    public static void SetColor(Entity entity, Vector3 color) => Native.aether_entity_material_set_color(entity.Id, color);
    public static void SetMetallic(Entity entity, float metallic) => Native.aether_entity_material_set_metallic(entity.Id, metallic);
    public static void SetRoughness(Entity entity, float roughness) => Native.aether_entity_material_set_roughness(entity.Id, roughness);
    public static void SetEmissive(Entity entity, Vector3 emissive) => Native.aether_entity_material_set_emissive(entity.Id, emissive);
    public static Vector3 GetEmissive(Entity entity) => Native.aether_entity_material_get_emissive(entity.Id);
}
