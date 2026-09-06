using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One wire, persisted as its own small marker entity (transform + this script) - not a
/// list field on WireHub. The engine's script-property serializer stores exactly one
/// SCALAR value per named property (Float/Int/Bool/Vector3/String/Enum/Entity/Component -
/// see ScriptPropertyValue in Components.hpp); there is no list/array property type at all,
/// so a `List&lt;Wire&gt;` field could never survive a save/load in the first place. A
/// dedicated entity per wire sidesteps that entirely: Source/OutputField/Target/InputField
/// are four ordinary scalar properties, using the exact same Entity-ref and String property
/// kinds every other script already relies on (see Whisper's Title.scene.toml `t='entity'`
/// fields) - zero new engine capability needed.
///
/// Source/Target are the SAME positional-index-plus-serializer-remap Entity references
/// every cross-entity script link in the repo already uses (confirmed against
/// Title.scene.toml), not the scene's `node` id - `node` is not resolvable back to a live
/// entity from script at all (grepped Scene.cs/Entity.cs to confirm before choosing).
///
/// WireHub is the only thing that ever reads Source/Target's fields by name (via
/// reflection) - this script carries no wire-EVALUATION logic of its own. It does now own
/// a wire's VISUAL, a real drawn segment between the two endpoints - grepped
/// src/engine/rendering/ and managed/AetherCore/ for an existing line-renderer first:
/// Debug.DrawLine exists but is explicitly a dev-only gizmo (off by default,
/// one-frame immediate-mode - see its own doc comment), unsuitable for something a
/// player is meant to see; there is no persistent line/cylinder mesh primitive. A
/// stretched cube (World.CreateMesh("cube"), already used by PropSpawner/Emitter) is
/// the shape that already exists, so that is what this uses.
/// </summary>
public sealed class WireLink : EntityScript
{
    public Entity Source;
    public string OutputField = "";
    public Entity Target;
    public string InputField = "";

    /// <summary>The wire's cross-section, not its length - length is derived from
    /// Source/Target's current positions every frame.</summary>
    public float VisualThickness = 0.035f;

    /// <summary>Colour while the wire's last-propagated value read at or below 0.5 -
    /// a dim neutral so an "off" wire reads as background, not as broken.</summary>
    public Vector3 ColorOff = new(0.28f, 0.28f, 0.32f);

    /// <summary>Colour while the wire's last-propagated value read above 0.5 - warm and
    /// bright, so a live signal is visible at a glance without opening anything.</summary>
    public Vector3 ColorOn = new(1.0f, 0.75f, 0.15f);

    private WireHub? _hub;
    private bool _registered;
    private Entity _visual;

    public override void OnAttach()
    {
        TryRegister();
        EnsureVisual();
    }

    /// <summary>Retries every frame until WireHub is found and this link is registered.
    /// Not just an OnAttach-time lookup: WireHub is an ordinary scene entity with its own
    /// attach order, and this script's OnAttach can run before WireHub's own C# instance
    /// exists yet - confirmed live (Scene.Find("WireHub") finds the entity immediately,
    /// a plain ECS lookup, but GetScript&lt;WireHub&gt;() raced it and returned null on the
    /// very frame a scene full of pre-authored wires loads). A one-shot OnAttach lookup
    /// would leave a scene-loaded wire silently unregistered forever; this instead
    /// catches up the moment WireHub actually exists, then stops checking.</summary>
    public override void OnUpdate(float deltaTime)
    {
        if (!_registered)
        {
            TryRegister();
        }
    }

    private void TryRegister()
    {
        _hub = Scene.Find("WireHub").GetScript<WireHub>();
        if (_hub == null)
        {
            return;
        }
        _hub.Register(this);
        _registered = true;
    }

    public override void OnDetach()
    {
        _hub?.Unregister(this);
        // Entity.Destroy does NOT cascade to children - confirmed by reading
        // ScriptComponentSystem.cpp's pending-destroy flush: it calls World::Destroy on
        // exactly the one queued id, never ecs::DestroyHierarchy (that recursive walk is
        // only ever used for the network despawn path, NetExports.cpp's
        // QueueHierarchyDestroy). WireHub.PruneDead destroying this entity would
        // therefore leave the visual an orphaned line to nowhere if nothing else
        // destroyed it explicitly - this does, so it dies with the wire regardless of
        // how this entity came to be destroyed (PruneDead, the Inspector, a script).
        if (_visual.IsValid)
        {
            _visual.Destroy();
        }
    }

    /// <summary>Creates the wire's drawn segment once, parented under this entity so it
    /// is grouped the same way the WireLink itself is (see RuntimeContainers' own file
    /// comment on the Wires container) rather than adding a second loose root entity per
    /// wire. Started at zero scale so a frame where Source/Target have not resolved yet
    /// (a scene-loaded wire, the instant after CreateWire) renders nothing rather than a
    /// stray box at the origin.</summary>
    private void EnsureVisual()
    {
        if (_visual.IsValid)
        {
            return;
        }
        _visual = World.Create();
        _visual.Name = "Wire Visual";
        _visual.AddTransform();
        _visual.SetTransform(Vector3.Zero, Vector3.Zero, Vector3.Zero);
        _visual.AddMesh(World.CreateMesh("cube"));
        _visual.SetMaterialColor(ColorOff);
        // Derived from Source/Target, never itself worth saving - regenerated fresh by
        // this same OnAttach on every load. A transient CHILD under this (persistent)
        // parent only excludes the child from capture, not the parent: confirmed by
        // reading Hierarchy.hpp's HasAncestorWith, which checks the entity ITSELF first,
        // then walks upward - the direction that actually breaks persistence is a
        // PERSISTENT child under a TRANSIENT parent (the reason the Wires container
        // itself is built non-transient - see RuntimeContainers' own file comment).
        _visual.MarkTransient();
        _visual.SetParent(Self);
    }

    /// <summary>Repositions, rescales and reorients the wire's visual from Source/Target's
    /// CURRENT world positions. Called by WireHub once a frame (WireHub.OnUpdate),
    /// deliberately separate from Propagate()'s own call frequency - a device can move
    /// (grabbed, a Door sliding) on a frame where no signal changes at all, and
    /// Propagate() runs zero or more times a frame depending on what pressed it. The
    /// actual yaw/pitch/scale math now lives in <see cref="SegmentVisual"/> - Beam needed
    /// the same derivation a second time, so it moved out from under this one caller
    /// rather than being copied.</summary>
    public void UpdateVisualTransform()
    {
        if (!_visual.IsValid || !World.IsValid(Source) || !World.IsValid(Target))
        {
            return;
        }
        SegmentVisual.Orient(_visual, Source.Position, Target.Position, VisualThickness);
    }

    /// <summary>Recolours the wire from its last-propagated value. Called by WireHub's
    /// own Propagate() loop right where it already reads this value to write into the
    /// target's input field, so this costs nothing extra to compute, only to apply.</summary>
    public void SetVisualValue(float value)
    {
        if (_visual.IsValid)
        {
            _visual.SetMaterialColor(value > 0.5f ? ColorOn : ColorOff);
        }
    }
}
