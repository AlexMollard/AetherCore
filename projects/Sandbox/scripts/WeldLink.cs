using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One weld, persisted as its own small marker entity - the exact shape ToolGun's own
/// file header specified back when the constraint API did not exist yet and the mode
/// had to be deferred: Source/Target-shaped Entity fields (not a list - see WireLink's
/// own file comment on why the script-property serializer has no array type), parented
/// under <see cref="RuntimeContainers.Get("Welds", transient: false)"/>, never
/// MarkTransient()'d itself, so save/load serialises the two endpoints and this script's
/// OnUpdate rebuilds the constraint fresh from them - handles are meaningless across a
/// reload (PhysicsSystem mints new ones on load), the endpoints are what persist.
///
/// The constraint is created in OnUpdate, not OnAttach, for the same reason a
/// just-created WireLink's fields are applied a frame late (ToolGun's own comment on
/// AddScript only queuing the attach): at attach time Source/Target are still default.
/// Created exactly once - a refusal is final, never retried every frame.
/// </summary>
public sealed class WeldLink : EntityScript
{
    public Entity Source;
    public Entity Target;
    /// <summary>The weld bead's cross-section - a thin dark line joining the two body
    /// centres, so a welded pair reads as joined at a glance without a bespoke mesh.</summary>
    public float VisualThickness = 0.05f;

    public Vector3 Color = new(0.45f, 0.42f, 0.38f);

    public float RejectFlashSeconds = 0.25f;

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
        // wires: Entity.Destroy does not cascade (confirmed twice, independently -
        // World.cpp:54 unregisters the root and destroys exactly one entity; only the
        // network despawn path walks the hierarchy), so a dead endpoint would leave
        // this marker and its bead as a line to nowhere that a mid-Play save then
        // captures forever. World.IsValid, NOT Entity.IsValid - the latter is only
        // Id != 0 and happily reports a destroyed entity as fine (a stale handle's
        // id slot can even be reused). Distinguished from "fields not applied yet" by
        // Entity.IsValid: default/unset is pending, set-but-world-dead is dead. Runs
        // every frame before anything else here, the same cadence as PruneDead - the
        // marker is gone by the end of the death frame, one full frame before a save's
        // capture pass could ever see it.
        bool sourceDied = Source.IsValid && !World.IsValid(Source);
        bool targetDied = Target.IsValid && !World.IsValid(Target);
        if (sourceDied || targetDied)
        {
            Log.Warn($"[Sandbox] WeldLink ({Self.Name}): endpoint {(sourceDied ? Source.Name : Target.Name)} was destroyed - removing the weld with it.");
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
        if (_visual.IsValid && endpointsAlive)
        {
            SegmentVisual.Orient(_visual, Source.Position, Target.Position, VisualThickness);
        }
    }

    public override void OnDetach()
    {
        // Same reasoning as WireLink.OnDetach: World::Destroy DOES cascade to children
        // now (engine-side fix), so this is not compensating for that. It compensates
        // for OnDetach's OTHER trigger - scene.remove_script / the Inspector's "Remove
        // Component" removes just this script from a surviving entity, leaving
        // _visual orphaned with nothing else to clean it up. DestroyConstraint is a
        // no-op on an unknown/already-cleaned handle (its own doc comment), so this is
        // safe both when the marker is removed by hand and when the refusal path
        // never made one.
        Physics.DestroyConstraint(_handle);
        if (_visual.IsValid)
        {
            _visual.Destroy();
        }
    }

    private void TryCreate()
    {
        _attempted = true;
        _handle = Physics.CreateWeld(Source, Target);
        if (_handle != 0)
        {
            return;
        }
        // 0 = refused (dead endpoint by the time this ran, or CanControl says the body
        // is not this caller's - see CreateWeld's own doc comment). Surface it exactly
        // like Wire mode's rejection - visibly, never as a silent no-op - then remove
        // this marker so a dead weld never persists into a save.
        Log.Warn($"[Sandbox] WeldLink ({Self.Name}): Physics.CreateWeld refused - endpoint dead or not owned by this peer; removing the weld.");
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

    /// <summary>Same shape as WireLink.EnsureVisual: a derived-data child, zero-scale
    /// until the endpoints resolve, transient (the visual is regenerated on every load
    /// by this same script) under this persistent parent.</summary>
    private void EnsureVisual()
    {
        if (_visual.IsValid)
        {
            return;
        }
        _visual = World.Create();
        _visual.Name = "Weld Visual";
        _visual.AddTransform();
        _visual.SetTransform(Vector3.Zero, Vector3.Zero, Vector3.Zero);
        _visual.AddMesh(World.CreateMesh("cube"));
        _visual.SetMaterialColor(Color);
        _visual.MarkTransient();
        _visual.SetParent(Self);
    }
}
