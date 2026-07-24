using System.Numerics;
using System.Runtime.InteropServices;

namespace AetherCore;

/// <summary>Body type for 2D rigid bodies (matches the engine's Body2DType).</summary>
public enum Body2DType
{
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
}

/// <summary>
/// A single 2D raycast/circle-cast hit. Blittable - matches the engine's RaycastHit2D.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct RaycastHit2D
{
    private int _hit;
    public Vector2 Point;
    public Vector2 Normal;
    public float Fraction;
    private uint _entity;

    /// <summary>True when the cast struck a body.</summary>
    public bool DidHit => _hit != 0;

    /// <summary>The entity that was hit (invalid when <see cref="DidHit"/> is false).</summary>
    public Entity Entity => new(_entity);
}

/// <summary>
/// 2D physics control (Box2D-backed; exclusive to 2D scenes). Adding a collider
/// queues the body for the physics system to bake; velocity, forces and queries
/// go through the live body. Angles are radians, positive counter-clockwise.
/// </summary>
public static class Physics2D
{
    // ── Bodies ────────────────────────────────────────────────────────────────
    public static void AddBoxBody(Entity entity, Vector2 size, Body2DType type = Body2DType.Dynamic)
        => Native.aether_physics2d_add_box(entity.Id, size, (int)type);

    public static void AddCircleBody(Entity entity, float radius, Body2DType type = Body2DType.Dynamic)
        => Native.aether_physics2d_add_circle(entity.Id, radius, (int)type);

    /// <summary>Capsule aligned to local Y; <paramref name="height"/> is end-to-end.</summary>
    public static void AddCapsuleBody(Entity entity, float radius, float height, Body2DType type = Body2DType.Dynamic)
        => Native.aether_physics2d_add_capsule(entity.Id, radius, height, (int)type);

    /// <summary>Turn the entity's collider into a trigger (sensor) or back.</summary>
    public static void SetTrigger(Entity entity, bool trigger)
        => Native.aether_physics2d_set_trigger(entity.Id, trigger ? 1 : 0);

    // ── Velocity / forces ─────────────────────────────────────────────────────
    public static void SetLinearVelocity(Entity entity, Vector2 velocity)
        => Native.aether_physics2d_set_linear_velocity(entity.Id, velocity);

    public static Vector2 GetLinearVelocity(Entity entity) => Native.aether_physics2d_get_linear_velocity(entity.Id);

    /// <summary>Angular velocity in radians per second, positive counter-clockwise.</summary>
    public static void SetAngularVelocity(Entity entity, float radiansPerSec)
        => Native.aether_physics2d_set_angular_velocity(entity.Id, radiansPerSec);

    public static float GetAngularVelocity(Entity entity) => Native.aether_physics2d_get_angular_velocity(entity.Id);

    /// <summary>Continuous force (mass-dependent), applied over the step. Use in OnUpdate.</summary>
    public static void AddForce(Entity entity, Vector2 force) => Native.aether_physics2d_add_force(entity.Id, force);

    /// <summary>Instantaneous change in momentum (mass-dependent). Use for jumps/hits.</summary>
    public static void AddImpulse(Entity entity, Vector2 impulse) => Native.aether_physics2d_add_impulse(entity.Id, impulse);

    public static void AddTorque(Entity entity, float torque) => Native.aether_physics2d_add_torque(entity.Id, torque);

    public static void AddAngularImpulse(Entity entity, float impulse) => Native.aether_physics2d_add_angular_impulse(entity.Id, impulse);

    public static void SetGravityScale(Entity entity, float scale) => Native.aether_physics2d_set_gravity_scale(entity.Id, scale);

    /// <summary>Let this body fall through one-way platforms for the next <paramref name="seconds"/>
    /// (0 cancels). Long enough to clear the platform is plenty - a quarter second or so.</summary>
    public static void SetDropThrough(Entity entity, float seconds) => Native.aether_physics2d_set_drop_through(entity.Id, seconds);

    // ── Queries ───────────────────────────────────────────────────────────────
    /// <summary><paramref name="direction"/> must be normalized.</summary>
    public static RaycastHit2D Raycast(Vector2 origin, Vector2 direction, float maxDistance)
        => Native.aether_physics2d_raycast(origin, direction, maxDistance);

    public static RaycastHit2D CircleCast(Vector2 origin, float radius, Vector2 direction, float maxDistance)
        => Native.aether_physics2d_circlecast(origin, radius, direction, maxDistance);

    /// <summary>True if a solid (two-way) tile covers this world point - correct even deep inside a
    /// solid block, where an overlap query against the hollow tilemap chain colliders reports nothing.</summary>
    public static bool IsPointSolid(Vector2 point) => Native.aether_physics2d_is_point_solid(point) != 0;

    /// <summary>Every entity whose body overlaps a circle at <paramref name="center"/>.</summary>
    public static Entity[] OverlapCircle(Vector2 center, float radius)
        => Overlaps(Native.aether_physics2d_overlap_circle(center, radius));

    /// <summary>Every entity whose body contains <paramref name="point"/>.</summary>
    public static Entity[] OverlapPoint(Vector2 point)
        => Overlaps(Native.aether_physics2d_overlap_point(point));

    /// <summary>Every entity whose body overlaps the axis-aligned box.</summary>
    public static Entity[] OverlapArea(Vector2 min, Vector2 max)
        => Overlaps(Native.aether_physics2d_overlap_aabb(min, max));

    private static Entity[] Overlaps(int count)
    {
        var result = new Entity[count];
        for (int i = 0; i < count; i++)
        {
            result[i] = new Entity(Native.aether_physics2d_overlap_at(i));
        }
        return result;
    }

    // ── Collision / trigger events ────────────────────────────────────────────
    /// <summary>Start recording collision/trigger events on this entity.</summary>
    public static void EnableEvents(Entity entity) => Native.aether_physics2d_enable_events(entity.Id);

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
        int count = Native.aether_physics2d_event_count(id, kind);
        var result = new Entity[count];
        for (int i = 0; i < count; i++)
        {
            result[i] = new Entity(Native.aether_physics2d_event_at(id, kind, i));
        }
        return result;
    }
}
