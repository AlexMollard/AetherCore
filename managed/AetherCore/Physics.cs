using System.Numerics;

namespace AetherCore;

/// <summary>
/// Physics control. Shape descriptors are queued for the physics system to
/// realize; velocity and state go through the live rigid body.
/// </summary>
public static class Physics
{
    public static void AddBoxBody(Entity entity, Vector3 halfExtents, bool dynamic = true)
        => Native.aether_physics_add_box(entity.Id, halfExtents, dynamic ? 1 : 0);

    public static void AddSphereBody(Entity entity, float radius, bool dynamic = true)
        => Native.aether_physics_add_sphere(entity.Id, radius, dynamic ? 1 : 0);

    public static void AddCapsuleBody(Entity entity, float halfHeight, float radius, bool dynamic = true)
        => Native.aether_physics_add_capsule(entity.Id, halfHeight, radius, dynamic ? 1 : 0);

    public static void SetLinearVelocity(Entity entity, Vector3 velocity)
        => Native.aether_physics_set_linear_velocity(entity.Id, velocity);

    public static Vector3 GetLinearVelocity(Entity entity) => Native.aether_physics_get_linear_velocity(entity.Id);

    /// <summary>Interpolated world position from the physics state.</summary>
    public static Vector3 GetPosition(Entity entity) => Native.aether_physics_get_position(entity.Id);

    public static Vector3 GetScale(Entity entity) => Native.aether_physics_get_scale(entity.Id);

    public static bool DebugDrawEnabled
    {
        get => Native.aether_physics_is_debug_enabled() != 0;
        set => Native.aether_physics_set_debug_enabled(value ? 1 : 0);
    }
}
