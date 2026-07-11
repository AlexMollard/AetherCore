using System.Numerics;
using System.Runtime.InteropServices;

namespace AetherCore;

/// <summary>
/// A single raycast/spherecast hit. Blittable - matches the engine's RaycastHit.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct RaycastHit
{
    private int _hit;
    public Vector3 Position;
    public Vector3 Normal;
    public float Fraction;
    private uint _entity;

    /// <summary>True when the cast struck a body.</summary>
    public bool DidHit => _hit != 0;

    /// <summary>The entity that was hit (invalid when <see cref="DidHit"/> is false).</summary>
    public Entity Entity => new(_entity);
}

/// <summary>
/// Physics control. Adding a collider (+ rigid body) queues the body for the
/// physics system to bake; velocity, forces and state go through the live body.
/// </summary>
public static class Physics
{
    // ── Bodies ────────────────────────────────────────────────────────────────
    public static void AddBoxBody(Entity entity, Vector3 halfExtents, bool dynamic = true)
        => Native.aether_physics_add_box(entity.Id, halfExtents, dynamic ? 1 : 0);

    public static void AddSphereBody(Entity entity, float radius, bool dynamic = true)
        => Native.aether_physics_add_sphere(entity.Id, radius, dynamic ? 1 : 0);

    public static void AddCapsuleBody(Entity entity, float halfHeight, float radius, bool dynamic = true)
        => Native.aether_physics_add_capsule(entity.Id, halfHeight, radius, dynamic ? 1 : 0);

    /// <summary>Freeze rotation about the given world axes (keeps a character upright).</summary>
    public static void FreezeRotation(Entity entity, bool x, bool y, bool z)
        => Native.aether_physics_freeze_rotation(entity.Id, x ? 1 : 0, y ? 1 : 0, z ? 1 : 0);

    // ── Velocity / forces ─────────────────────────────────────────────────────
    public static void SetLinearVelocity(Entity entity, Vector3 velocity)
        => Native.aether_physics_set_linear_velocity(entity.Id, velocity);

    public static Vector3 GetLinearVelocity(Entity entity) => Native.aether_physics_get_linear_velocity(entity.Id);

    public static void SetAngularVelocity(Entity entity, Vector3 velocity)
        => Native.aether_physics_set_angular_velocity(entity.Id, velocity);

    public static Vector3 GetAngularVelocity(Entity entity) => Native.aether_physics_get_angular_velocity(entity.Id);

    /// <summary>Continuous force (mass-dependent), applied over the step. Use in OnUpdate.</summary>
    public static void AddForce(Entity entity, Vector3 force) => Native.aether_physics_add_force(entity.Id, force);

    /// <summary>Instantaneous change in momentum (mass-dependent). Use for jumps/hits.</summary>
    public static void AddImpulse(Entity entity, Vector3 impulse) => Native.aether_physics_add_impulse(entity.Id, impulse);

    public static void AddTorque(Entity entity, Vector3 torque) => Native.aether_physics_add_torque(entity.Id, torque);

    public static void AddAngularImpulse(Entity entity, Vector3 impulse) => Native.aether_physics_add_angular_impulse(entity.Id, impulse);

    /// <summary>Interpolated world position from the physics state.</summary>
    public static Vector3 GetPosition(Entity entity) => Native.aether_physics_get_position(entity.Id);

    public static Vector3 GetScale(Entity entity) => Native.aether_physics_get_scale(entity.Id);

    // ── Queries ───────────────────────────────────────────────────────────────
    public static RaycastHit Raycast(Vector3 origin, Vector3 direction, float maxDistance)
        => Native.aether_physics_raycast(origin, direction, maxDistance);

    public static RaycastHit SphereCast(Vector3 origin, Vector3 direction, float radius, float maxDistance)
        => Native.aether_physics_spherecast(origin, direction, radius, maxDistance);

    /// <summary>Every entity whose body overlaps a sphere at <paramref name="center"/>.</summary>
    public static Entity[] OverlapSphere(Vector3 center, float radius)
    {
        int count = Native.aether_physics_overlap_sphere(center, radius);
        var result = new Entity[count];
        for (int i = 0; i < count; i++)
        {
            result[i] = new Entity(Native.aether_physics_overlap_at(i));
        }
        return result;
    }

    // ── Collision / trigger events ────────────────────────────────────────────
    /// <summary>Start recording collision/trigger events on this entity.</summary>
    public static void EnableEvents(Entity entity) => Native.aether_physics_enable_events(entity.Id);

    /// <summary>Bodies whose solid contact began this frame.</summary>
    public static Entity[] GetCollisionEnter(Entity entity) => Events(entity.Id, 0);
    /// <summary>Bodies whose solid contact ended this frame.</summary>
    public static Entity[] GetCollisionExit(Entity entity) => Events(entity.Id, 1);
    /// <summary>Trigger/sensor overlaps that began this frame.</summary>
    public static Entity[] GetTriggerEnter(Entity entity) => Events(entity.Id, 2);
    /// <summary>Trigger/sensor overlaps that ended this frame.</summary>
    public static Entity[] GetTriggerExit(Entity entity) => Events(entity.Id, 3);
    /// <summary>Everything currently in contact (solid or trigger).</summary>
    public static Entity[] GetOverlapping(Entity entity) => Events(entity.Id, 4);

    private static Entity[] Events(uint id, int kind)
    {
        int count = Native.aether_physics_event_count(id, kind);
        var result = new Entity[count];
        for (int i = 0; i < count; i++)
        {
            result[i] = new Entity(Native.aether_physics_event_at(id, kind, i));
        }
        return result;
    }

    public static bool DebugDrawEnabled
    {
        get => Native.aether_physics_is_debug_enabled() != 0;
        set => Native.aether_physics_set_debug_enabled(value ? 1 : 0);
    }
}
