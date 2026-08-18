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

    /// <summary>World-space scale.</summary>
    public Vector3 Scale
    {
        get => Native.aether_get_scale(Id);
        set => Native.aether_set_scale(Id, value);
    }

    public string Name
    {
        get => World.GetName(Id);
        set => Native.aether_set_name(Id, value);
    }

    /// <summary>Sets the full TRS at once; the entity's subtree follows the delta.</summary>
    public void SetTransform(Vector3 position, Vector3 eulerDegrees, Vector3 scale)
        => Native.aether_set_transform(Id, position, eulerDegrees, scale);

    /// <summary>
    /// Destroy this entity. Deferred Unity-style: the entity is removed at the
    /// end of the current frame's script update, never mid-callback, so code
    /// after this call still sees it briefly.
    /// </summary>
    public void Destroy() => Native.aether_entity_destroy(Id);

    // ── Hierarchy ───────────────────────────────────────────────────────────────

    /// <summary>The parent entity (invalid if this is a root).</summary>
    public Entity Parent => new(Native.aether_entity_get_parent(Id));

    /// <summary>Re-parent under <paramref name="parent"/> (pass an invalid entity to detach to root).</summary>
    public void SetParent(Entity parent) => Native.aether_entity_set_parent(Id, parent.Id);

    /// <summary>Number of direct children.</summary>
    public int ChildCount => Native.aether_entity_child_count(Id);

    /// <summary>The i-th direct child (invalid if out of range).</summary>
    public Entity GetChild(int index) => new(Native.aether_entity_child_at(Id, index));

    // ── Active state ────────────────────────────────────────────────────────────

    /// <summary>True when neither this entity nor any ancestor is disabled - i.e.
    /// it participates in simulation and rendering.</summary>
    public bool ActiveInHierarchy => Native.aether_entity_is_active(Id) != 0;

    /// <summary>Enable or disable this entity (and its subtree) in the scene.</summary>
    public void SetActive(bool active) => Native.aether_entity_set_active(Id, active ? 1 : 0);

    /// <summary>A typed reference to one of this entity's components, e.g.
    /// <c>entity.Get&lt;RigidBodyRef&gt;()</c>. The wrapper's operations no-op if the
    /// entity doesn't actually carry the component.</summary>
    public T Get<T>() where T : IComponentRef => (T)Activator.CreateInstance(typeof(T), this)!;

    /// <summary>
    /// The live script instance of type <typeparamref name="T"/> attached to this
    /// entity, or <c>null</c> if it carries no such script - Unity's
    /// <c>GetComponent&lt;T&gt;()</c>. Works for any entity, so a collision or
    /// trigger callback can query <c>other</c>, not just <c>Self</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// An entity may carry the same script type more than once; the first one
    /// attached is returned.
    /// </para>
    /// <para>
    /// Attach ordering: scripts on an entity are created and attached one at a
    /// time, in the order they are listed on the entity, so during
    /// <see cref="EntityScript.OnAttach"/> only the scripts listed BEFORE the
    /// caller exist. Code that must not depend on that order should look the
    /// sibling up in <see cref="EntityScript.OnUpdate"/> instead, where every
    /// script on the entity is live.
    /// </para>
    /// </remarks>
    public T? GetScript<T>() where T : EntityScript => ScriptInstances.Find<T>(Id);

    /// <summary>Exclude this entity (and subtree) from scene serialization.</summary>
    public void MarkTransient() => Native.aether_mark_transient(Id);

    /// <summary>
    /// Unity-style persistence: this entity (and its subtree) survives
    /// <see cref="Scene.Load"/>, keeping its components and attached script
    /// instances alive across the switch. Gameplay-only: editor scene loads
    /// and stopping Play always reset the world, and nothing is saved into
    /// scene files. If the destination scene authors its own copy of a
    /// persistent actor, resolve the duplicate in the script (in OnAttach:
    /// when a live instance already exists, destroy Self) - only the
    /// surviving copy runs, so every scene can author one and loading any
    /// scene directly still works.
    /// </summary>
    public void DontDestroyOnLoad() => Native.aether_mark_transient(Id);

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
