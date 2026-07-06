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
}

/// <summary>Factory for shared authored materials.</summary>
public static class Material
{
    /// <summary>Create a shared material; edit it, bind it to many entities.</summary>
    public static MaterialId Make(Vector3 color, float metallic, float roughness)
        => new(Native.aether_make_material(color, metallic, roughness));
}
