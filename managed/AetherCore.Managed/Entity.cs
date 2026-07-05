using System;
using System.Numerics;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

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

    public bool Equals(Entity other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is Entity e && Equals(e);
    public override int GetHashCode() => (int)Id;
    public static bool operator ==(Entity a, Entity b) => a.Id == b.Id;
    public static bool operator !=(Entity a, Entity b) => a.Id != b.Id;
}
