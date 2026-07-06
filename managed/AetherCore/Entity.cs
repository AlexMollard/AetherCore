using System;
using System.Numerics;

namespace AetherCore;

/// <summary>
/// A handle to an engine entity (a 32-bit id; 0 is invalid). Properties and
/// methods forward to the native ECS; the struct itself holds no state.
///
/// Note on math types: the engine stores transforms as glm column-major matrices.
/// System.Numerics.Vector3 is byte-identical to glm::vec3, so vectors cross the
/// boundary as a raw copy with no conversion.
/// </summary>
public readonly struct Entity : IEquatable<Entity>
{
    public readonly uint Id;

    public Entity(uint id) => Id = id;

    public bool IsValid => Id != 0;

    /// <summary>World-space translation.</summary>
    public Vector3 Position
    {
        get => Native.aether_get_position(Id);
        set => Native.aether_set_position(Id, value);
    }

    /// <summary>World-space rotation as YXZ euler angles in degrees.</summary>
    public Vector3 EulerDegrees
    {
        get => Native.aether_get_euler(Id);
        set => Native.aether_set_euler(Id, value);
    }

    /// <summary>World-space scale (read-only; set via <see cref="SetTransform"/>).</summary>
    public Vector3 Scale => Native.aether_get_scale(Id);

    public string Name
    {
        get => World.GetName(Id);
        set => Native.aether_set_name(Id, value);
    }

    /// <summary>Sets the full TRS at once; the entity's subtree follows the delta.</summary>
    public void SetTransform(Vector3 position, Vector3 eulerDegrees, Vector3 scale)
        => Native.aether_set_transform(Id, position, eulerDegrees, scale);

    public void Destroy() => Native.aether_entity_destroy(Id);

    /// <summary>Exclude this entity (and subtree) from scene serialization.</summary>
    public void MarkTransient() => Native.aether_mark_transient(Id);

    /// <summary>Give the entity an identity transform (needed before SetTransform).</summary>
    public void AddTransform() => Native.aether_add_transform(Id);

    public bool HasTransform => Native.aether_has_transform(Id) != 0;

    public void RemoveTransform() => Native.aether_remove_transform(Id);

    // ── Behaviors ─────────────────────────────────────────────────────────────
    public void AddBob(float amplitude, float frequency, float phase = 0f) => Native.aether_add_bob(Id, amplitude, frequency, phase);

    public void AddSpin(Vector3 eulerDegreesPerSecond) => Native.aether_add_spin(Id, eulerDegreesPerSecond);

    public void AddOrbit(Vector3 center, float radius, float speedDegrees, float startAngleDegrees = 0f, float yawOffsetDegrees = 0f, float height = 0f)
        => Native.aether_add_orbit(Id, center, radius, speedDegrees, startAngleDegrees, yawOffsetDegrees, height);

    public void AddMaterialPulse(Vector3 emissiveA, Vector3 emissiveB, float frequency)
        => Native.aether_add_material_pulse(Id, emissiveA, emissiveB, frequency);

    /// <summary>Attach a C# script to this entity by type name.</summary>
    public void AddScript(string typeName) => Native.aether_add_script(Id, typeName);

    // ── Meshes / models ───────────────────────────────────────────────────────
    /// <summary>Load a glTF model and spawn its meshes as children of this entity.</summary>
    public void LoadModel(string path) => Native.aether_load_model(Id, path);

    public void AddMesh(MeshHandle mesh) => Native.aether_add_mesh(Id, mesh.Value);

    // ── Materials ─────────────────────────────────────────────────────────────
    /// <summary>Paint a solid material (content-addressed; identical values share a slot).</summary>
    public void SetMaterial(Vector3 color, float metallic, float roughness) => Native.aether_set_material(Id, color, metallic, roughness);

    /// <summary>Paint with the default surface response.</summary>
    public void SetMaterialColor(Vector3 color) => Native.aether_set_material_color(Id, color);

    /// <summary>Bind a shared authored material to this entity.</summary>
    public void BindMaterial(MaterialId material) => Native.aether_bind_material(Id, material.Value);

    /// <summary>Set the albedo texture from a VFS path (missing paths show magenta).</summary>
    public void SetMaterialTexture(string path) => Native.aether_set_material_texture(Id, path);

    /// <summary>Per-entity, copy-on-write material field edits (e.g. <c>Material.SetColor(...)</c>).</summary>
    public MaterialEditor Material => new(Id);

    // ── Tags ──────────────────────────────────────────────────────────────────
    public void AddTag(TagId tag) => Native.aether_tag_add(Id, tag.Value);

    public bool HasTag(TagId tag) => Native.aether_tag_has(Id, tag.Value) != 0;

    public void RemoveTag(TagId tag) => Native.aether_tag_remove(Id, tag.Value);

    public bool Equals(Entity other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is Entity e && Equals(e);
    public override int GetHashCode() => (int)Id;
    public static bool operator ==(Entity a, Entity b) => a.Id == b.Id;
    public static bool operator !=(Entity a, Entity b) => a.Id != b.Id;
}
