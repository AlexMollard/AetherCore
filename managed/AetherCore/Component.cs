using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>
/// Generic access to any reflected component on an entity, by catalog name and field
/// name - the same names the Inspector shows and the scene file stores.
///
/// Every component declared in the engine's reflection tables is reachable this way, so
/// a component does not need a hand-written script API before gameplay code can touch
/// it. Where a typed wrapper exists (<see cref="SpinRef"/>, <see cref="RigidBody2DRef"/>
/// and friends) prefer it: same mechanism underneath, but the field names are checked by
/// the compiler instead of at runtime.
///
/// <code>
/// ComponentAccess spin = Self.Component("Spin");
/// if (!spin.Exists) { spin.Add(); }
/// spin.SetVector3("euler_deg_per_sec", new Vector3(0, 90, 0));
/// </code>
///
/// Getters return the supplied fallback when the component or field is absent, so a
/// missing component reads as a default rather than throwing mid-frame.
/// </summary>
public readonly struct ComponentAccess
{
    /// <summary>The entity the component lives on.</summary>
    public Entity Owner { get; }

    /// <summary>Catalog name, e.g. "Spin" or "Sprite Renderer".</summary>
    public string Type { get; }

    public ComponentAccess(Entity owner, string type)
    {
        Owner = owner;
        Type = type;
    }

    /// <summary>Whether the entity currently carries this component.</summary>
    public bool Exists => Native.aether_component_has(Owner.Id, Type) != 0;

    /// <summary>
    /// Add it with its declared defaults. Does nothing if it is already present, so this
    /// is safe to call from OnAttach across a hot reload. False if the component cannot
    /// be added (unknown name, or one the engine attaches itself).
    /// </summary>
    public bool Add() => Native.aether_component_add(Owner.Id, Type) != 0;

    /// <summary>Remove it. False if the component name is not known.</summary>
    public bool Remove() => Native.aether_component_remove(Owner.Id, Type) != 0;

    // ── Scalars ───────────────────────────────────────────────────────────────────

    public float GetFloat(string field, float fallback = 0.0f)
        => Native.aether_component_get_number(Owner.Id, Type, field, out double v) != 0 ? (float)v : fallback;

    public bool SetFloat(string field, float value)
        => Native.aether_component_set_number(Owner.Id, Type, field, value) != 0;

    public int GetInt(string field, int fallback = 0)
        => Native.aether_component_get_number(Owner.Id, Type, field, out double v) != 0 ? (int)v : fallback;

    public bool SetInt(string field, int value)
        => Native.aether_component_set_number(Owner.Id, Type, field, value) != 0;

    public bool GetBool(string field, bool fallback = false)
        => Native.aether_component_get_number(Owner.Id, Type, field, out double v) != 0 ? v != 0.0 : fallback;

    public bool SetBool(string field, bool value)
        => Native.aether_component_set_number(Owner.Id, Type, field, value ? 1.0 : 0.0) != 0;

    // ── Vectors and colours ───────────────────────────────────────────────────────

    public Vector2 GetVector2(string field, Vector2 fallback = default)
        => Native.aether_component_get_vector(Owner.Id, Type, field, out Vector4 v) != 0 ? new Vector2(v.X, v.Y) : fallback;

    public bool SetVector2(string field, Vector2 value)
        => Native.aether_component_set_vector(Owner.Id, Type, field, new Vector4(value.X, value.Y, 0.0f, 0.0f)) != 0;

    public Vector3 GetVector3(string field, Vector3 fallback = default)
        => Native.aether_component_get_vector(Owner.Id, Type, field, out Vector4 v) != 0 ? new Vector3(v.X, v.Y, v.Z) : fallback;

    public bool SetVector3(string field, Vector3 value)
        => Native.aether_component_set_vector(Owner.Id, Type, field, new Vector4(value, 0.0f)) != 0;

    public Vector4 GetVector4(string field, Vector4 fallback = default)
        => Native.aether_component_get_vector(Owner.Id, Type, field, out Vector4 v) != 0 ? v : fallback;

    public bool SetVector4(string field, Vector4 value)
        => Native.aether_component_set_vector(Owner.Id, Type, field, value) != 0;

    // ── Strings ───────────────────────────────────────────────────────────────────

    public unsafe string GetString(string field, string fallback = "")
    {
        Span<byte> buffer = stackalloc byte[512];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_component_get_string(Owner.Id, Type, field, ptr, buffer.Length);
            return written >= 0 ? Encoding.UTF8.GetString(ptr, written) : fallback;
        }
    }

    public bool SetString(string field, string value)
        => Native.aether_component_set_string(Owner.Id, Type, field, value) != 0;
}
