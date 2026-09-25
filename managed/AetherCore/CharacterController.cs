using System.Numerics;

namespace AetherCore;

/// <summary>
/// 3D character control: a Jolt CharacterVirtual-backed swept capsule with slope/step/ground
/// handling, stepped once per fixed physics substep (see PhysicsSystem::StepCharacters).
/// Adding a capsule queues it for the physics system to bake, the same way Physics.AddBoxBody
/// queues a rigid body; movement goes through Move/SetVelocity, never Self.Position.
/// </summary>
public static class CharacterController
{
    /// <summary>Adds a capsule character controller to the entity.</summary>
    public static void Add(Entity entity, float radius, float halfHeight)
        => Native.aether_character_add(entity.Id, radius, halfHeight);

    /// <summary>
    /// Sets the desired horizontal velocity (direction * speed). The controller owns the
    /// vertical component (gravity, ground-follow, jump) - any vertical component in
    /// <paramref name="direction"/> is ignored. Persists until the next Move or SetVelocity call.
    /// </summary>
    public static void Move(Entity entity, Vector3 direction, float speed)
        => Native.aether_character_move(entity.Id, direction, speed);

    /// <summary>
    /// Raw full velocity override (horizontal and vertical) for a step the script wants to
    /// fully own - knockback, a scripted move, and the like.
    /// </summary>
    public static void SetVelocity(Entity entity, Vector3 velocity)
        => Native.aether_character_set_velocity(entity.Id, velocity);

    /// <summary>Current velocity the controller is integrating (read back after physics steps).</summary>
    public static Vector3 GetVelocity(Entity entity) => Native.aether_character_get_velocity(entity.Id);

    /// <summary>
    /// Requests a jump: applied on the next physics substep if the character is grounded at
    /// that moment, then dropped. No jump buffering - a request while airborne is lost.
    /// </summary>
    public static void Jump(Entity entity, float speed) => Native.aether_character_jump(entity.Id, speed);

    /// <summary>True if the character was standing on walkable ground as of the last physics step.</summary>
    public static bool IsGrounded(Entity entity) => Native.aether_character_is_grounded(entity.Id) != 0;

    /// <summary>Surface normal of the ground the character is standing on (world up when airborne).</summary>
    public static Vector3 GetGroundNormal(Entity entity) => Native.aether_character_get_ground_normal(entity.Id);

    /// <summary>The entity the character is standing on or touching as ground, including steep
    /// ground (invalid when airborne) - for surface rules such as deadly ground.</summary>
    public static Entity GetGroundEntity(Entity entity) => new(Native.aether_character_get_ground_entity(entity.Id));
}
