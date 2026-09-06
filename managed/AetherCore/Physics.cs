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

/// <summary>A rigid body's simulation ownership - values match the engine's
/// PhysicsMotionType (PhysicsComponents.hpp) exactly, crossing the ABI as a raw
/// int32.</summary>
public enum PhysicsMotionType
{
    /// <summary>Never moves.</summary>
    Static = 0,

    /// <summary>Script/network-authored: PushKinematicTargets reads whatever this
    /// entity's Transform says every physics step and solves the body's velocity to
    /// arrive there over that step (Jolt's own MoveKinematic), so it genuinely
    /// shoves whatever it touches - unlike a Static body's frozen pose, and unlike a
    /// plain SetPosition teleport, which was measured live in this engine imparting
    /// zero (not "small") momentum into a body it passed through.</summary>
    Kinematic = 1,

    /// <summary>Jolt's own solver decides position every step.</summary>
    Dynamic = 2,
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

    public static void AddCylinderBody(Entity entity, float halfHeight, float radius, bool dynamic = true)
        => Native.aether_physics_add_cylinder(entity.Id, halfHeight, radius, dynamic ? 1 : 0);

    /// <summary>A convex hull built from <paramref name="meshSource"/>'s baked vertices
    /// (a project:// model path). Usable on a body of any motion type - Jolt reduces
    /// the source mesh to a genuine convex surface, discarding interior points.</summary>
    public static void AddConvexHullBody(Entity entity, string meshSource, bool dynamic = true)
        => Native.aether_physics_add_convex_hull(entity.Id, meshSource, dynamic ? 1 : 0);

    /// <summary>The exact triangles of <paramref name="meshSource"/> - for concave
    /// geometry a hull cannot represent (e.g. the open space under a table). Must be
    /// Static: Jolt requires it and the engine rejects (with a warning, not silently)
    /// anything else.</summary>
    public static void AddMeshBody(Entity entity, string meshSource)
        => Native.aether_physics_add_mesh(entity.Id, meshSource, 0);

    /// <summary>Change an EXISTING body's motion type in place - Jolt's own
    /// BodyInterface::SetMotionType, no destroy/rebuild needed. The one script path to
    /// a Kinematic body: every Add*Body call above only ever takes Static or Dynamic.
    /// Refused (no effect, no error) on an entity this caller does not own, the same
    /// authority gate every other write below already enforces - see CanControl in
    /// PhysicsExports.cpp.</summary>
    public static void SetMotionType(Entity entity, PhysicsMotionType motionType)
        => Native.aether_physics_set_motion_type(entity.Id, (int)motionType);

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

    /// <summary>Instantaneous change in momentum applied at a WORLD POINT rather than the
    /// centre of mass, so it imparts spin - what makes a thrown prop tumble instead of
    /// sliding flat. The physics gun's throw primitive.</summary>
    public static void AddImpulseAtPoint(Entity entity, Vector3 impulse, Vector3 point)
        => Native.aether_physics_add_impulse_at_point(entity.Id, impulse, point);

    // ── Per-body material / activation ───────────────────────────────────────
    /// <summary>Multiplies scene gravity for this body alone: 1 is normal, 0 floats - what a
    /// grab controller sets while holding a prop so it doesn't fight the hold.</summary>
    public static void SetGravityFactor(Entity entity, float factor) => Native.aether_physics_set_gravity_factor(entity.Id, factor);

    /// <summary>How much sliding contact resists. 0 is ice, 1 is rubber.</summary>
    public static void SetFriction(Entity entity, float friction) => Native.aether_physics_set_friction(entity.Id, friction);

    /// <summary>Bounciness. 0 lands dead, 1 returns all the energy it arrived with.</summary>
    public static void SetRestitution(Entity entity, float restitution) => Native.aether_physics_set_restitution(entity.Id, restitution);

    /// <summary>Force the body awake or asleep - a grab controller's other primitive: wake a
    /// sleeping prop the instant it is picked up, since a sleeping body otherwise ignores
    /// velocity/impulse writes until something else wakes it (see RigidBodyComponent's own
    /// allow_sleeping tooltip).</summary>
    public static void SetBodyActive(Entity entity, bool active) => Native.aether_physics_set_body_active(entity.Id, active ? 1 : 0);

    public static bool IsBodyActive(Entity entity) => Native.aether_physics_is_body_active(entity.Id) != 0;

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

    // ── Constraints (weld/rope) ───────────────────────────────────────────────
    /// <summary>Welds <paramref name="entity"/> rigidly to <paramref name="target"/> at
    /// their current relative pose - a default/invalid <paramref name="target"/> pins
    /// it to the world instead
    /// (a fixed anchor, nothing to weld to). Returns a handle for
    /// <see cref="DestroyConstraint"/>; 0 means the call was refused (dead entity, or
    /// <paramref name="entity"/> is not this caller's to weld - see CanControl in
    /// PhysicsExports.cpp). An entity can hold more than one weld/rope at once - three
    /// props welded in a line gives the middle one two.</summary>
    public static uint CreateWeld(Entity entity, Entity target)
        => Native.aether_physics_create_weld(entity.Id, target.Id);

    /// <summary>Ropes <paramref name="entity"/> to <paramref name="target"/> (or to the
    /// world) at <paramref name="worldAnchor"/>, holding them exactly
    /// <paramref name="restLength"/> apart once built - a rigid rope, not an elastic
    /// one. Returns a handle for <see cref="DestroyConstraint"/>; 0 means refused, same
    /// conditions as <see cref="CreateWeld"/>.</summary>
    public static uint CreateRope(Entity entity, Entity target, Vector3 worldAnchor, float restLength)
        => Native.aether_physics_create_rope(entity.Id, target.Id, worldAnchor, restLength);

    /// <summary>Removes a weld or rope by the handle <see cref="CreateWeld"/>/
    /// <see cref="CreateRope"/> returned. A no-op on an unknown or already-destroyed
    /// handle (e.g. its other endpoint died first and cleaned it up already) - a tool
    /// gun releasing a contraption piece by piece never has to check for that itself.</summary>
    public static void DestroyConstraint(uint handle) => Native.aether_physics_destroy_constraint(handle);
}
