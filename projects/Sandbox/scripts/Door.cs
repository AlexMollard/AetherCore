using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Wiremod's second sink: proves a wire input can drive MOTION, not just a colour
/// (Lamp). Self must already carry a Collider + Rigid Body (any shape - add both in
/// whatever scene/prefab places a Door); this script forces the body Kinematic and
/// drives its position, it does not create the physics body itself (same convention as
/// Lamp's own file comment: it drives an existing light, not one it creates).
///
/// KINEMATIC, NOT A TELEPORT. A straight Entity.Position write on a Static or Dynamic
/// body either does nothing physically or gets fought back next step - PhysicsSystem's
/// own SyncTransforms owns a Dynamic body's position every step, and a Static body is
/// never revisited by PushKinematicTargets at all (PhysicsSystem.cpp, ~line 732).
/// PushKinematicTargets is the ONLY path that turns a script's own Transform write into
/// a real JPH::BodyInterface::MoveKinematic call: every physics step it reads a
/// Kinematic body's CURRENT TransformComponent and solves the body's velocity to arrive
/// there over that one step - the exact mechanism its own comment calls out for a
/// "network-driven crate sliding through a stack of props actually shoves them" instead
/// of passing through. A plain teleport was measured live this session imparting
/// -2.3e-11 residual momentum into whatever the body passed through - not "small",
/// zero. So OnAttach calls Physics.SetMotionType(Self, Kinematic), and OnUpdate steps
/// Self.Position toward the open/closed target by Speed*deltaTime rather than snapping
/// to it - that incremental write per frame IS the velocity-based path, because
/// PushKinematicTargets re-derives velocity from whatever the transform says
/// immediately before every physics step.
///
/// Physics.SetMotionType (Physics.cs/PhysicsExports.cpp) is a genuinely new export,
/// added alongside this script: PhysicsSystem::SetBodyMotionType already existed and
/// already handled Kinematic, but NetworkContext::SyncSimulationAuthority was its only
/// caller - nothing surfaced it to script before. Before this export existed, the only
/// script-reachable route to it was Rigid Body's "motion" field via
/// ComponentAccess.SetInt (the same generic reflection path Lamp uses for Point
/// Light's "intensity") - correct, but pinning the enum's raw int ordinal by hand in
/// script is exactly the kind of thing a real export exists to avoid.
/// </summary>
public sealed class Door : EntityScript
{
    /// <summary>INPUT. Greater than 0.5 reads as "open" - a level, like Lamp's Enable,
    /// not a pulse. Wire a Latch's Out into this to have a momentary Button hold the
    /// door open (see Gate's own file comment on Latch).</summary>
    public float Enable;

    /// <summary>How far (and which way) the door slides from its closed pose, in
    /// world-space units - e.g. (0, 2.5, 0) slides it up two and a half metres.</summary>
    public Vector3 OpenOffset = new(0.0f, 2.5f, 0.0f);

    /// <summary>Slide speed in units/second.</summary>
    public float Speed = 2.0f;

    private Vector3 _closedPosition;
    private bool _ready;

    public override void OnAttach()
    {
        if (!Self.Component("Collider").Exists || !Self.Component("Rigid Body").Exists)
        {
            Log.Warn("[Sandbox] Door: this entity has no Collider/Rigid Body - add both in the scene/prefab. Door drives an existing kinematic body, it does not create one.");
            return;
        }

        Physics.SetMotionType(Self, PhysicsMotionType.Kinematic);
        _closedPosition = Self.Position;
        _ready = true;
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_ready)
        {
            return;
        }

        Vector3 target = Enable > 0.5f ? _closedPosition + OpenOffset : _closedPosition;
        Vector3 current = Self.Position;
        Vector3 toTarget = target - current;
        float distance = toTarget.Length();
        float step = Speed * deltaTime;

        Self.Position = distance <= step ? target : current + toTarget / distance * step;
    }
}
