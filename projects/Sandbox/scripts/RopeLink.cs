using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One rope, persisted as its own small marker entity - the WireLink shape ToolGun's
/// own header specified for it (Entity endpoint fields, non-transient, rebuild the
/// constraint from the endpoints on load because handles are meaningless across a
/// reload). What a rope adds over a weld is the two anchor points: captured in world
/// space at creation and persisted as ordinary Vector3 properties, they set the rest
/// length (the actual distance between the anchors at creation - a rope is born exactly
/// taut, never pre-stretched or pre-slack) and are the world anchor
/// <see cref="Physics.CreateRope"/> takes.
///
/// The visual is the persisted-rope answer to the same question Beam answers for the
/// transient case - Beam.cs itself is transient-only by design (see its own file
/// comment), so this reuses <see cref="SegmentVisual"/> directly the way WireLink does:
/// one stretched-cube segment between the two anchors, re-anchored every frame from
/// each body's CURRENT transform. AnchorA/AnchorB are stored as body-local offsets at
/// creation and rotated by each body's current Euler angles on the way out, so the rope
/// stays glued to the point you clicked rather than drifting to the body centre as the
/// prop tumbles (System.Numerics provides the quaternion; the Euler order assumed here
/// is the same X-pitch/Y-yaw/Z-roll the camera writes in FirstPersonPlayer).
/// </summary>
public sealed class RopeLink : EntityScript
{
    public Entity Source;
    public Entity Target;

    /// <summary>World-space anchor on Source at creation - the point the first click
    /// landed on. Persisted; the constraint is rebuilt with this as its world anchor.</summary>
    public Vector3 AnchorA;

    /// <summary>World-space anchor on Target at creation - the second click's point.
    /// RestLength is the distance between the two anchors, so this field is technically
    /// derived; persisted anyway because it is the honest record of where the rope was
    /// tied, and it is what the visual re-anchors from.</summary>
    public Vector3 AnchorB;

    /// <summary>Distance between the two anchors at creation - what
    /// <see cref="Physics.CreateRope"/> holds them exactly apart.</summary>
    public float RestLength;

    public float VisualThickness = 0.045f;

    public Vector3 Color = new(0.55f, 0.45f, 0.30f);

    /// <summary>Same refusal-flash contract as WeldLink's - visible, never silent.</summary>
    public float RejectFlashSeconds = 0.25f;

    private Vector3 _localA;
    private Vector3 _localB;
    private bool _localsCaptured;

    private Entity _visual;
    private bool _attempted;
    private uint _handle;
    private float _rejectRemaining;

    public override void OnAttach()
    {
        EnsureVisual();
    }

    public override void OnUpdate(float deltaTime)
    {
        // Dead-endpoint sweep - the identical defect WireHub.PruneDead solves for
        // wires, and the reason this runs before anything else here: Entity.Destroy
        // does not cascade (confirmed twice, independently - World.cpp:54 unregisters
        // the root and destroys exactly one entity; only the network despawn path
        // walks the hierarchy), so a dead endpoint would leave this marker and its
        // rope as a line to nowhere that a mid-Play save then captures forever.
        // World.IsValid, NOT Entity.IsValid - the latter is only Id != 0 and happily
        // reports a destroyed entity as fine (a stale handle's id slot can even be
        // reused). Distinguished from "fields not applied yet" by Entity.IsValid:
        // default/unset is pending, set-but-world-dead is dead. Every frame, the same
        // cadence as PruneDead - the marker is gone by the end of the death frame,
        // one full frame before a save's capture pass could ever see it.
        bool sourceDied = Source.IsValid && !World.IsValid(Source);
        bool targetDied = Target.IsValid && !World.IsValid(Target);
        if (sourceDied || targetDied)
        {
            Log.Warn($"[Sandbox] RopeLink ({Self.Name}): endpoint {(sourceDied ? Source.Name : Target.Name)} was destroyed - removing the rope with it.");
            Self.Destroy();
            return;
        }

        if (_rejectRemaining > 0.0f)
        {
            _rejectRemaining -= deltaTime;
            if (_rejectRemaining <= 0.0f)
            {
                ClearRejectFlash();
                Self.Destroy();
            }
            return;
        }

        bool endpointsAlive = World.IsValid(Source) && World.IsValid(Target);
        if (!_attempted && endpointsAlive)
        {
            TryCreate();
        }
        if (_visual.IsValid && endpointsAlive && _localsCaptured)
        {
            SegmentVisual.Orient(_visual, WorldAnchor(Source, _localA), WorldAnchor(Target, _localB), VisualThickness);
        }
    }

    public override void OnDetach()
    {
        // Same as WeldLink.OnDetach: the bead must not outlive the marker - not because
        // World::Destroy fails to cascade (it does), but because OnDetach also fires
        // when just this script is removed from a surviving entity (scene.remove_script
        // / the Inspector), which leaves nothing else to clean up _visual.
        // DestroyConstraint no-ops on an unknown/already-cleaned handle.
        Physics.DestroyConstraint(_handle);
        if (_visual.IsValid)
        {
            _visual.Destroy();
        }
    }

    private void TryCreate()
    {
        _attempted = true;
        // Local offsets are captured from the same world anchors the constraint is
        // built with, so visual and simulation agree on where the rope is tied even
        // before the first body moves.
        _localA = AnchorA - Source.Position;
        _localB = AnchorB - Target.Position;
        _localsCaptured = true;

        _handle = Physics.CreateRope(Source, Target, AnchorA, RestLength);
        if (_handle != 0)
        {
            return;
        }
        Log.Warn($"[Sandbox] RopeLink ({Self.Name}): Physics.CreateRope refused - endpoint dead or not owned by this peer; removing the rope.");
        Vector3 reject = new(0.85f, 0.15f, 0.15f);
        if (World.IsValid(Source))
        {
            Source.Material.SetEmissive(reject);
        }
        if (World.IsValid(Target))
        {
            Target.Material.SetEmissive(reject);
        }
        _rejectRemaining = RejectFlashSeconds;
    }

    private void ClearRejectFlash()
    {
        if (World.IsValid(Source))
        {
            Source.Material.SetEmissive(Vector3.Zero);
        }
        if (World.IsValid(Target))
        {
            Target.Material.SetEmissive(Vector3.Zero);
        }
    }

    /// <summary>Re-applies a body's current world transform to its creation-time local
    /// anchor offset. Ignores the body's scale: these are props at unit scale in this
    /// project, and a scaled rope anchor is not a case the tool offers today.</summary>
    private static Vector3 WorldAnchor(Entity body, Vector3 local)
    {
        Vector3 euler = body.EulerDegrees;
        Quaternion rotation = Quaternion.CreateFromYawPitchRoll(euler.Y * (MathF.PI / 180.0f), euler.X * (MathF.PI / 180.0f), euler.Z * (MathF.PI / 180.0f));
        return body.Position + Vector3.Transform(local, rotation);
    }

    private void EnsureVisual()
    {
        if (_visual.IsValid)
        {
            return;
        }
        _visual = World.Create();
        _visual.Name = "Rope Visual";
        _visual.AddTransform();
        _visual.SetTransform(Vector3.Zero, Vector3.Zero, Vector3.Zero);
        _visual.AddMesh(World.CreateMesh("cube"));
        _visual.SetMaterialColor(Color);
        _visual.MarkTransient();
        _visual.SetParent(Self);
    }
}
