using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// GMod's tool gun: one aim raycast, one fire key, a current mode that changes what the
/// fire key does. This used to be WiringTool - wiring is now <see cref="ToolMode.Wire"/>,
/// generalised rather than left beside a second tool, because everything a mode needs
/// (aim raycast, target validation, a pending-source state machine, a persisted result
/// entity) already existed here for wiring alone. Adding a mode is a new
/// <see cref="ToolMode"/> value plus one branch in <see cref="TryToolAction"/> - not a new
/// script, the same "small table, not an interface" reasoning WireHub's own file comment
/// gives for device roles.
///
/// MODES BACKED TODAY: <see cref="ToolMode.Wire"/> (unchanged from WiringTool - the
/// two-press pending/connect/reject flow), <see cref="ToolMode.Light"/> (adds/configures a
/// Point Light on whatever is hit, via the same generic reflected-component path Lamp
/// already uses to drive one), <see cref="ToolMode.Colour"/> (one
/// <see cref="Entity.SetMaterialColor"/> call), <see cref="ToolMode.Remove"/> (destroys
/// whatever "grabbable"-tagged prop is hit - gated on that tag, the same one
/// PhysicsGun/BotGrabber use, so a stray click can never remove world geometry, a wiring
/// device, or the player).
///
/// WELD AND ROPE ARE DELIBERATELY NOT IN <see cref="ToolMode"/> YET. Both need a fixed or
/// distance physics constraint, and there is no constraint API exposed to C# today
/// (grepped managed/AetherCore/Physics.cs - AddBoxBody/AddSphereBody/impulses/velocity
/// only, nothing joint-shaped). Do not stub a mode that silently does nothing when fired -
/// add the enum value and its branch together, the moment the engine side lands. THE
/// PERSISTENCE SHAPE FOR THEM IS ALREADY SPECIFIED, so whoever adds them does not have to
/// re-derive it: follow <see cref="WireLink"/> exactly - a small marker entity per
/// weld/rope holding Entity references to both attachment points (Source/Target-shaped
/// fields, not a list - see WireLink's own file comment on why), parented under
/// <c>RuntimeContainers.Get("Welds", transient: false)</c> / <c>"Ropes"</c>
/// (transient: false, like "Wires" and unlike "Beams" - a weld/rope must survive save the
/// same way a wire does), never MarkTransient()'d itself. A rope's visible segment is
/// exactly a <see cref="Beam"/>-shaped problem (a curve between two live points) but
/// Beam.cs itself is transient-only by design (see its own file comment) - a persisted
/// rope's visual should reuse <see cref="SegmentVisual"/> directly the way WireLink does,
/// not reuse the transient Beam class.
///
/// LIGHT AND COLOUR NEED NO NEW PERSISTENCE WORK. A Point Light added to an entity, or a
/// material colour changed on one, are ordinary reflected-component writes - the scene
/// serializer already captures every component on every non-transient entity (see
/// RuntimeContainers' own file comment on ecs::HasSceneTransientAncestor for the one way
/// that can go wrong, which does not apply here: this never creates a new entity, only
/// edits fields on whatever was hit). If the hit entity itself is transient (a
/// PropSpawner-spawned prop, say), the light/colour change is exactly as ephemeral as the
/// prop it is on, which is correct, not a gap.
///
/// MODE CYCLING: mouse scroll (<see cref="Input.ScrollDelta"/>.Y), the same input surface
/// PhysicsGun already reads for hold-distance while something is held - checked and
/// yielded to explicitly (see <see cref="HandleModeCycle"/>) rather than fought over,
/// since there is no cross-script input-ownership concept in this engine (see
/// docs/mcp-setup.md's own note on the related editor/game version of that gap) for two
/// scripts on the same entity to arbitrate through.
///
/// Attached to the Main Camera alongside PhysicsGun/SpawnMenu/PropSpawner (NetPlayerRig.cs
/// - was WiringTool there, now ToolGun), so Self is the camera and the same
/// forward-ray-from-eye-height origin applies with no extra indirection - and gated on
/// _player's authority for the same reason those three are (Self carries no
/// NetworkIdentity of its own; see PhysicsGun.OnUpdate's own comment).
/// </summary>
public sealed class ToolGun : EntityScript
{
    public enum ToolMode
    {
        Wire,
        Light,
        Colour,
        Remove,
    }

    public float MaxRange = 10.0f;
    public float RayStartOffset = 0.35f;
    public Vector3 PendingTint = new(0.85f, 0.75f, 0.15f);
    public Vector3 RejectTint = new(0.85f, 0.15f, 0.15f);
    public float RejectFlashSeconds = 0.25f;

    /// <summary>The key that presses a Button or flips a Lever dead ahead - the same
    /// key InteractPromptText names, so a rebind here never leaves the HUD prompt
    /// showing a stale letter. Independent of the tool gun's own mode/fire key below;
    /// "use whatever is directly ahead" is ordinary gameplay, not a tool action.</summary>
    public Key InteractKey = Key.G;

    /// <summary>Fires the current mode's action - was "wire_tool"/T when this script was
    /// WiringTool and only had the one thing to fire. Kept on the same key rather than
    /// moved, so nothing already muscle-memoried on T breaks.</summary>
    public Key ToolFireKey = Key.T;

    public float LightIntensity = 20.0f;
    public float LightRadius = 8.0f;
    public Vector3 LightColor = Vector3.One;

    public Vector3 PaintColor = new(0.2f, 0.6f, 0.9f);

    private static readonly ToolMode[] Modes = (ToolMode[])Enum.GetValues(typeof(ToolMode));

    private int _modeIndex;
    public ToolMode Mode => Modes[_modeIndex];

    /// <summary>What UiHud shows for the current mode - built from the enum name and
    /// ToolFireKey rather than a hardcoded string, so neither a rebind nor a renamed
    /// mode can leave this stale.</summary>
    public string ModeLabel => $"[{ToolFireKey}] {Mode} tool (scroll to change)";

    private Entity _player;
    private Entity _pendingSource;
    private string _pendingField = "";
    private Entity _flashTarget;
    private float _flashRemaining;

    /// <summary>Camera-attached first-person model (asset-pipeline's
    /// ToolGunViewmodel.glb - Kenney Blaster Kit, no collider, not a physics prop).
    /// Repositioned every frame from Self's current forward/right rather than parented
    /// with a "local" offset, because Entity transform setters in this engine are
    /// world-space always (SetParent never composes local coordinates on its own - see
    /// RuntimeContainers' own file comment on why re-parenting never moves anything).
    /// Vertical offset deliberately omitted: deriving Up from Forward x Right needs a
    /// sign this session had no live Editor to confirm, and a same-height placement is a
    /// safe default in the meantime - adjust once someone can see it. Never save-worthy
    /// (PhysicsGun.EnsureHud's own comment on the same point for its crosshair canvas),
    /// so this is MarkTransient()'d like every other runtime-only visual in this
    /// project.</summary>
    private Entity _viewmodel;

    private const string ViewmodelPath = "project://assets/models/Viewmodels/ToolGunViewmodel.glb";
    private const float ViewmodelForwardOffset = 0.5f;
    private const float ViewmodelRightOffset = 0.22f;

    /// <summary>Whatever the aim ray is currently over that TryInteract would actually
    /// press or flip - Button or Lever, an invalid entity when nothing qualifies.
    /// Recomputed once a frame in OnUpdate rather than only when "interact" is pressed,
    /// so UiHud can poll it every frame (same idiom as PhysicsGun.Held) to show/hide the
    /// "[key] Use" prompt the instant the player looks onto or away from something
    /// interactable - MaxRange already bounds Aim()'s raycast, so any hit here is by
    /// definition in range.</summary>
    public Entity InteractTarget { get; private set; }

    /// <summary>The exact text UiHud shows over InteractTarget - built from InteractKey
    /// rather than a hardcoded "[G]" so it never goes stale if that binding changes.</summary>
    public string InteractPromptText => $"[{InteractKey}] Use";

    // AddScript only queues a ScriptEntry for the script-component system to pick up on
    // a later pass (confirmed by reading aether_add_script's native implementation - it
    // pushes a ScriptEntry and returns, it does not instantiate anything); a
    // just-created WireLink entity's script does not exist yet on the same frame
    // CreateWire calls AddScript, so its four fields have to be applied once it does.
    private Entity _pendingWireEntity;
    private Entity _pendingWireSource;
    private string _pendingWireOutput = "";
    private Entity _pendingWireTarget;
    private string _pendingWireInput = "";

    public override void OnAttach()
    {
        _player = Self.Parent;
        InputActions.Register("interact", InteractKey);
        InputActions.Register("tool_fire", ToolFireKey);
        EnsureViewmodel();
    }

    public override void OnUpdate(float deltaTime)
    {
        // Owner-only, same reasoning as PhysicsGun/PropSpawner/SpawnMenu: _player, not
        // Self, because Self carries no NetworkIdentity and would read Net.HasAuthority
        // as unconditionally true otherwise. True offline, so single-player is
        // unaffected.
        if (!Net.HasAuthority(_player))
        {
            InteractTarget = default;
            return;
        }
        PollPendingWire();
        if (GetScript<SpawnMenu>() is { IsOpen: true })
        {
            if (_pendingSource.IsValid)
            {
                CancelPending();
            }
            InteractTarget = default;
            return;
        }

        TickFlash(deltaTime);
        RefreshInteractTarget();
        HandleModeCycle();
        UpdateViewmodel();

        if (InputActions.IsPressed("interact"))
        {
            TryInteract();
        }
        if (InputActions.IsPressed("tool_fire"))
        {
            TryToolAction();
        }
    }

    /// <summary>Scroll cycles the mode, yielding to PhysicsGun's own use of scroll
    /// (push/pull distance) while it has something held - checked by reading its public
    /// Held property, not by contesting the input, since there is no ownership
    /// arbitration between two scripts on the same entity in this engine.</summary>
    private void HandleModeCycle()
    {
        if (GetScript<PhysicsGun>() is { Held.IsValid: true })
        {
            return;
        }
        float scroll = Input.ScrollDelta.Y;
        if (scroll > 0.0f)
        {
            _modeIndex = (_modeIndex + 1) % Modes.Length;
        }
        else if (scroll < 0.0f)
        {
            _modeIndex = (_modeIndex - 1 + Modes.Length) % Modes.Length;
        }
        else
        {
            return;
        }
        // A pending wire source from the mode just left behind would otherwise sit
        // tinted forever with no way to cancel it short of switching back to Wire and
        // re-clicking it.
        CancelPending();
    }

    /// <summary>Spawns the held viewmodel once, as a sibling of Self rather than a
    /// child of it - LoadModel spawns a model's meshes as children of the entity it is
    /// called on, and Self already carries this script plus PhysicsGun/SpawnMenu/
    /// PropSpawner, none of which expect a stray mesh subtree appearing under the
    /// camera. A dedicated entity keeps this script's own visual cleanly separate.</summary>
    private void EnsureViewmodel()
    {
        if (_viewmodel.IsValid)
        {
            return;
        }
        _viewmodel = World.Create();
        _viewmodel.Name = "Tool Gun Viewmodel";
        _viewmodel.AddTransform();
        _viewmodel.LoadModel(ViewmodelPath);
        _viewmodel.MarkTransient();
    }

    /// <summary>Repositions/reorients the viewmodel from Self's CURRENT world transform
    /// every frame - see this field's own comment for why that is a follow, not a
    /// parent-local offset.</summary>
    private void UpdateViewmodel()
    {
        if (!_viewmodel.IsValid)
        {
            return;
        }
        Vector3 forward = Camera.GetForward(Self);
        Vector3 right = Camera.GetRight(Self);
        Vector3 position = Self.Position + forward * ViewmodelForwardOffset + right * ViewmodelRightOffset;
        _viewmodel.SetTransform(position, Self.EulerDegrees, Vector3.One);
    }

    private void TryToolAction()
    {
        switch (Mode)
        {
            case ToolMode.Wire:
                TryWireStep();
                break;
            case ToolMode.Light:
                TryAttachLight();
                break;
            case ToolMode.Colour:
                TryPaint();
                break;
            case ToolMode.Remove:
                TryRemove();
                break;
        }
    }

    /// <summary>Recomputes InteractTarget from the same aim ray TryInteract itself
    /// would use - see InteractTarget's own field comment for why this runs every
    /// frame rather than only on a keypress.</summary>
    private void RefreshInteractTarget()
    {
        RaycastHit hit = Aim();
        InteractTarget = hit.DidHit && hit.Entity.IsValid && IsInteractable(hit.Entity)
            ? hit.Entity
            : default;
    }

    private static bool IsInteractable(Entity e) => e.GetScript<Button>() != null || e.GetScript<Lever>() != null;

    private void TickFlash(float deltaTime)
    {
        if (_flashRemaining <= 0.0f)
        {
            return;
        }
        _flashRemaining -= deltaTime;
        if (_flashRemaining <= 0.0f && _flashTarget.IsValid)
        {
            _flashTarget.Material.SetEmissive(Vector3.Zero);
            _flashTarget = default;
        }
    }

    private RaycastHit Aim()
    {
        Vector3 forward = Camera.GetForward(Self);
        Vector3 origin = Self.Position + forward * RayStartOffset;
        return Physics.Raycast(origin, forward, MaxRange);
    }

    private void TryInteract()
    {
        if (InteractTarget.GetScript<Button>() is { } button)
        {
            button.Interact();
            return;
        }
        InteractTarget.GetScript<Lever>()?.Interact();
    }

    /// <summary>Adds a Point Light with this script's configured defaults if the hit
    /// entity does not already carry one - idempotent, so firing twice on the same prop
    /// does not add a second light or reset one already tuned by hand. Uses the exact
    /// reflected-component path Lamp.cs's own file comment verified end to end
    /// (Self.Component/ComponentAccess), just calling Add() first where Lamp only ever
    /// warns on a missing light - Lamp drives an existing one, this creates one.</summary>
    private void TryAttachLight()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }
        ComponentAccess light = hit.Entity.Component("Point Light");
        if (!light.Exists && !light.Add())
        {
            Log.Warn($"[Sandbox] ToolGun: '{hit.Entity.Name}' cannot carry a Point Light (Lighting3D feature off, or this entity kind cannot host one).");
            return;
        }
        light.SetFloat("intensity", LightIntensity);
        light.SetFloat("radius", LightRadius);
        light.SetVector3("color", LightColor);
    }

    private void TryPaint()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }
        hit.Entity.SetMaterialColor(PaintColor);
    }

    /// <summary>Gated on the same "grabbable" tag PhysicsGun/BotGrabber already use to
    /// tell a prop from world geometry (PhysicsGun.cs's own file comment on why a tag
    /// exists instead of a script-side "is this dynamic" query) - so Remove can only ever
    /// take a spawned/arena prop, never a wall, a wiring device, or the player, without
    /// this script needing an exclusion list of its own. A non-grabbable hit flashes
    /// RejectTint exactly like Wire mode's own rejection - a miss is always visible,
    /// never silent, in every mode.</summary>
    private void TryRemove()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }
        if (!Tags.Has(hit.Entity, Tags.Create("grabbable")))
        {
            FlashReject(hit.Entity);
            return;
        }
        hit.Entity.Destroy();
    }

    // ── Wire mode (unchanged from WiringTool) ───────────────────────────────────────

    private static string? OutputFieldOf(Entity e)
    {
        if (e.GetScript<Button>() != null)
        {
            return "Pressed";
        }
        if (e.GetScript<Lever>() != null)
        {
            return "Out";
        }
        if (e.GetScript<Gate>() != null)
        {
            return "Out";
        }
        return null;
    }

    private static string? InputFieldOf(Entity e, WireHub? hub)
    {
        if (e.GetScript<Lamp>() != null || e.GetScript<Door>() != null || e.GetScript<Emitter>() != null)
        {
            return "Enable";
        }
        if (e.GetScript<Gate>() != null)
        {
            bool aTaken = hub != null && hub.IsInputConnected(e, "A");
            return aTaken ? "B" : "A";
        }
        return null;
    }

    private void TryWireStep()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }

        if (!_pendingSource.IsValid)
        {
            string? output = OutputFieldOf(hit.Entity);
            if (output == null)
            {
                return;
            }
            _pendingSource = hit.Entity;
            _pendingField = output;
            _pendingSource.Material.SetEmissive(PendingTint);
            return;
        }

        if (hit.Entity == _pendingSource)
        {
            CancelPending();
            return;
        }

        WireHub? hub = Scene.Find("WireHub").GetScript<WireHub>();
        string? input = InputFieldOf(hit.Entity, hub);
        if (input == null || hub == null || hub.WouldCreateCycle(_pendingSource, hit.Entity))
        {
            FlashReject(hit.Entity);
            CancelPending();
            return;
        }

        CreateWire(_pendingSource, _pendingField, hit.Entity, input);
        CancelPending();
    }

    private void CancelPending()
    {
        if (_pendingSource.IsValid)
        {
            _pendingSource.Material.SetEmissive(Vector3.Zero);
        }
        _pendingSource = default;
        _pendingField = "";
    }

    private void FlashReject(Entity target)
    {
        target.Material.SetEmissive(RejectTint);
        _flashTarget = target;
        _flashRemaining = RejectFlashSeconds;
    }

    /// <summary>Applies a pending wire's four fields the first frame its WireLink
    /// instance actually exists (see the field comments on _pendingWireEntity for why
    /// that is not the same frame CreateWire ran).</summary>
    private void PollPendingWire()
    {
        if (!_pendingWireEntity.IsValid)
        {
            return;
        }
        WireLink? link = _pendingWireEntity.GetScript<WireLink>();
        if (link == null)
        {
            return;
        }
        link.Source = _pendingWireSource;
        link.OutputField = _pendingWireOutput;
        link.Target = _pendingWireTarget;
        link.InputField = _pendingWireInput;
        _pendingWireEntity = default;
    }

    private void CreateWire(Entity source, string outputField, Entity target, string inputField)
    {
        // Deliberately NOT MarkTransient() - a wire must survive save/load, that is the
        // whole point (see WireLink's own file comment on why it is a real scene entity
        // rather than a list field in the first place).
        Entity wireEntity = World.Create();
        wireEntity.Name = $"Wire ({outputField} -> {inputField})";
        wireEntity.AddTransform();
        // Grouped under one Hierarchy panel entry instead of flooding the scene root -
        // transient: false is not the default for a reason: a transient container would
        // silently exclude every wire parented under it from being saved at all, undoing
        // the persistence fix landed earlier today (see RuntimeContainers' own file
        // comment on ecs::HasSceneTransientAncestor).
        wireEntity.SetParent(RuntimeContainers.Get("Wires", transient: false));
        wireEntity.AddScript("WireLink");
        _pendingWireEntity = wireEntity;
        _pendingWireSource = source;
        _pendingWireOutput = outputField;
        _pendingWireTarget = target;
        _pendingWireInput = inputField;
    }
}
