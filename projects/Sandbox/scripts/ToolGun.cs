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
/// WELD AND ROPE ARE BACKED. This file's earlier revision deferred both modes because
/// no constraint API existed; Physics.CreateWeld/CreateRope/DestroyConstraint have since
/// landed (with save/reload serde - PhysicsSystem mints fresh handles on load), so both
/// modes follow exactly the persistence shape that revision specified: a two-click flow
/// spawning a <see cref="WeldLink"/>/<see cref="RopeLink"/> marker entity per
/// weld/rope - Entity endpoint fields, parented under
/// <c>RuntimeContainers.Get("Welds"/"Ropes", transient: false)</c>, never
/// MarkTransient()'d - so save/load serialises the endpoints and the marker's own
/// OnUpdate rebuilds the constraint from them. A rope's visible segment reuses
/// <see cref="SegmentVisual"/> directly the way WireLink does (Beam.cs itself is
/// transient-only by design - see its own file comment). A hit without a Rigid Body
/// flash-rejects in both modes, and a refused constraint (dead endpoint, or CanControl
/// says it is not this caller's - the 0 return both Create* calls document) flashes the
/// endpoints red from the marker script itself, which owns the refusal because it only
/// becomes known a frame after the second click, once the marker's fields are set.
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
/// scripts on the same entity to arbitrate through. RMB cancel (see
/// <see cref="HandleCancelInput"/>) yields to PhysicsGun's own RMB-throw the same way.
///
/// UX PASS: the mode/fire HUD line used to be static text with no pending-state
/// feedback, no live target-validity indicator, and no bound cancel (only re-clicking
/// the pending source itself, or switching mode, ever cleared one). <see cref="ModeLabelSuffix"/>
/// is now flow-state aware ("aim at a source" vs "aim at a target (RMB to cancel)"),
/// <see cref="RefreshHoverHighlight"/> tints whatever is currently under the crosshair
/// BEFORE a click for every locally-decidable accept/reject decision (see
/// <see cref="IsAcceptableNextClick"/>, the one predicate both the hover indicator and
/// the click handlers read), and every emissive tint in this file (pending, hover,
/// reject-flash) now goes through <see cref="Highlight"/>, the same mechanism
/// <see cref="PhysicsGun"/>'s held-prop tint uses - see that file's own doc comment for
/// why a shared mechanism instead of two hand-rolled ones, and Highlight.cs's own file
/// comment for the persistence hazard it was written to close.
///
/// Attached to the Main Camera alongside PhysicsGun/SpawnMenu/PropSpawner (NetPlayerRig.cs
/// - was WiringTool there, now ToolGun), so Self is the camera and the same
/// forward-ray-from-eye-height origin applies with no extra indirection - and gated on
/// _player's authority for the same reason those three are (Self carries no
/// NetworkIdentity of its own; see PhysicsGun.OnUpdate's own comment).
/// </summary>
public sealed class ToolGun : EntityScript
{
    public float MaxRange = 10.0f;
    public float RayStartOffset = 0.35f;
    public Vector3 PendingTint = new(0.85f, 0.75f, 0.15f);
    public Vector3 RejectTint = new(0.85f, 0.15f, 0.15f);
    public float RejectFlashSeconds = 0.25f;

    /// <summary>Live "would this click be accepted" indicator on whatever is CURRENTLY
    /// under the crosshair, for Wire/Weld/Rope/Remove (the four modes with an actual
    /// accept/reject decision) - shown BEFORE the click rather than only flashing
    /// <see cref="RejectTint"/> after a failed one, since a locally-decidable rejection
    /// (missing Rigid Body, no wireable field, not tagged grabbable) is knowable every
    /// frame just from aiming. <see cref="RejectTint"/> itself is kept for what can only
    /// be decided AT the click - Wire's cycle check needs a specific pending+target
    /// pair, and a Weld/Rope refusal from CreateWeld/CreateRope's own CanControl check
    /// only resolves host-side, after the fact.</summary>
    public Vector3 ValidTargetTint = new(0.3f, 0.85f, 0.35f);

    public enum ToolMode
    {
        Wire,
        Light,
        Colour,
        Remove,
        Weld,
        Rope,
    }

    /// <summary>The key that presses a Button or flips a Lever dead ahead - the same
    /// key InteractPromptSuffix's prompt shows an icon/bracket for, so a rebind here
    /// never leaves the HUD prompt showing a stale letter. Independent of the tool
    /// gun's own mode/fire key below; "use whatever is directly ahead" is ordinary
    /// gameplay, not a tool action. A read-only proxy over InputActions (rather than a
    /// plain field, as this used to be) so a rebind in Settings is reflected here and in
    /// UiHud's prompt the instant it happens, with nothing to fall out of sync.</summary>
    public Key InteractKey => InputActions.GetPrimary("interact");

    /// <summary>Fires the current mode's action - was "wire_tool"/T when this script was
    /// WiringTool and only had the one thing to fire. Kept on the same key rather than
    /// moved, so nothing already muscle-memoried on T breaks. Same read-only-proxy
    /// reasoning as InteractKey above.</summary>
    public Key ToolFireKey => InputActions.GetPrimary("tool_fire");

    public float LightIntensity = 20.0f;
    public float LightRadius = 8.0f;
    public Vector3 LightColor = Vector3.One;

    public Vector3 PaintColor = new(0.2f, 0.6f, 0.9f);

    private static readonly ToolMode[] Modes = (ToolMode[])Enum.GetValues(typeof(ToolMode));

    private int _modeIndex;
    public ToolMode Mode => Modes[_modeIndex];

    /// <summary>What UiHud shows for the current mode, minus the leading key - UiHud
    /// prepends an icon glyph (or "[ToolFireKey]" bracket text where this pack has no
    /// icon for that key - see InputGlyphs.GetGlyph) for the key itself, since an icon
    /// font and this project's ordinary UI font cannot mix within one string (see
    /// InputGlyphs' own file comment). Flow-state aware for the three two-click modes:
    /// "aim at a source" before the first click banks one, "aim at a target (RMB to
    /// cancel)" once it has - the task's own ask for a visible pending state and an
    /// advertised cancel, instead of the first click's only feedback being the pending
    /// tint itself. Escape is not the cancel key here on purpose: it is already the
    /// global pause-menu toggle (see UiPauseMenu.cs), and this engine has no
    /// cross-script input-ownership arbitration to make one key mean two things safely
    /// (see HandleCancelInput's own comment on how RMB itself yields to PhysicsGun).</summary>
    public string ModeLabelSuffix
    {
        get
        {
            if (HasPendingSource())
            {
                return $"{Mode} \u2014 aim at {SecondClickNoun(Mode)} (RMB to cancel)";
            }
            return Mode switch
            {
                ToolMode.Wire => "Wire \u2014 aim at a source (scroll to change tool)",
                ToolMode.Weld => "Weld \u2014 aim at the first body (scroll to change tool)",
                ToolMode.Rope => "Rope \u2014 aim at the first body (scroll to change tool)",
                ToolMode.Light => "Light \u2014 aim and fire to add/edit (scroll to change tool)",
                ToolMode.Colour => "Paint \u2014 aim and fire to recolour (scroll to change tool)",
                ToolMode.Remove => "Remove \u2014 aim at a prop and fire (scroll to change tool)",
                _ => $"{Mode} tool (scroll to change)",
            };
        }
    }

    private bool HasPendingSource() => Mode switch
    {
        ToolMode.Wire => _pendingSource.IsValid,
        ToolMode.Weld => _pendingWeldSource.IsValid,
        ToolMode.Rope => _pendingRopeSource.IsValid,
        _ => false,
    };

    private static string SecondClickNoun(ToolMode mode) => mode switch
    {
        ToolMode.Wire => "a target",
        _ => "the second body", // Weld, Rope - Wire is the only mode naming its two ends differently
    };

    private Entity _player;
    private Entity _pauseMenuEntity;
    private Entity _pendingSource;
    private Vector3 _pendingBaseline;
    private string _pendingField = "";
    private Entity _flashTarget;
    private Vector3 _flashBaseline;
    private float _flashRemaining;

    /// <summary>Whatever is currently under the crosshair and shown with
    /// <see cref="ValidTargetTint"/> because <see cref="IsAcceptableNextClick"/> says
    /// the next tool-fire would accept it - see <see cref="RefreshHoverHighlight"/>.
    /// Never the SAME entity as a pending source or the flash target (those already
    /// carry their own, stronger tint - see RefreshHoverHighlight's own guard).</summary>
    private Entity _hoverTarget;
    private Vector3 _hoverBaseline;

    /// <summary>Camera-attached first-person model (asset-pipeline's
    /// ToolGunViewmodel.glb - Kenney Blaster Kit, no collider, not a physics prop).
    /// Repositioned every frame from Self's current forward/right/up rather than
    /// parented with a "local" offset, because Entity transform setters in this engine
    /// are world-space always (SetParent never composes local coordinates on its own -
    /// see RuntimeContainers' own file comment on why re-parenting never moves
    /// anything). The up direction comes from <see cref="CameraBasis.Up"/> - this
    /// file's earlier revision omitted the vertical term because its sign was
    /// unconfirmed; the derivation in CameraBasis.cs closed that gap. Never save-worthy
    /// (PhysicsGun.EnsureHud's own comment on the same point for its crosshair canvas),
    /// so this is MarkTransient()'d like every other runtime-only visual in this
    /// project.</summary>
    private Entity _viewmodel;

    /// <summary>Whatever the aim ray is currently over that TryInteract would actually
    /// press or flip - Button or Lever, an invalid entity when nothing qualifies.
    /// Recomputed once a frame in OnUpdate rather than only when "interact" is pressed,
    /// so UiHud can poll it every frame (same idiom as PhysicsGun.Held) to show/hide the
    /// "[key] Use" prompt the instant the player looks onto or away from something
    /// interactable - MaxRange already bounds Aim()'s raycast, so any hit here is by
    /// definition in range.</summary>
    public Entity InteractTarget { get; private set; }
    private const string ViewmodelPath = "project://assets/models/Viewmodels/ToolGunViewmodel.glb";
    private const float ViewmodelForwardOffset = 0.5f;
    private const float ViewmodelRightOffset = 0.22f;

    /// <summary>Vertical offset along the camera's own up direction - NEGATIVE holds
    /// the gun below the view centre, the usual first-person look. The up vector's
    /// sign is derived from the engine's own basis rather than remembered, in
    /// <see cref="CameraBasis.Up"/> (the earlier revision of this file omitted the
    /// vertical term entirely because that sign was unconfirmed - see
    /// CameraBasis.cs's own header for the derivation and its citations).</summary>
    private const float ViewmodelUpOffset = -0.15f;

    /// <summary>The rest of the text UiHud shows over InteractTarget, after the icon/
    /// bracket UiHud prepends for InteractKey (see ModeLabelSuffix's comment on why
    /// the key portion is a separate widget, not part of this string).</summary>
    public string InteractPromptSuffix => "Use";

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
    // Weld/Rope pendings, same shape and same late-attach reasoning as the wire ones
    // above: the marker entity's script does not exist the frame CreateWeldMarker/
    // CreateRopeMarker calls AddScript, so its endpoint fields are applied once it does.
    private Entity _pendingWeldEntity;
    private Entity _pendingWeldSource;
    private Vector3 _pendingWeldBaseline;
    private Entity _pendingWeldTarget;
    private Entity _pendingRopeEntity;
    private Entity _pendingRopeSource;
    private Vector3 _pendingRopeBaseline;
    private Entity _pendingRopeTarget;
    private Vector3 _pendingRopeAnchorA;
    private Vector3 _pendingRopeAnchorB;

    public override void OnAttach()
    {
        _player = Self.Parent;
        _pauseMenuEntity = Scene.Find("NetSession");
        // Idempotent upsert, registered every attach - see PropSpawner's own file
        // comment on why (a fresh per-connection script re-hardcoding the compiled-in
        // default would silently undo a saved rebind on every reconnect). Registers
        // from SandboxSettings directly, not from InteractKey/ToolFireKey above - those
        // are now read-only proxies BACK onto this registration, so reading them here
        // before the first Register call would see Key.None.
        SandboxSettings.EnsureLoaded();
        InputActions.Register("interact", SandboxSettings.BoundKeys["interact"]);
        InputActions.Register("tool_fire", SandboxSettings.BoundKeys["tool_fire"]);
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
        PollPendingWeld();
        PollPendingRope();
        // SpawnMenu.IsOpen alone (the only gate this file used to check) missed the
        // pause menu entirely - UiPauseMenu.cs's own file comment lists PhysicsGun and
        // PropSpawner as needing this second gate alongside FirstPersonPlayer, but this
        // script (split out of WiringTool after that comment was written) never got
        // one: the aim ray, tool-fire, and the pending/hover highlights kept updating
        // while the game was paused.
        if (GetScript<SpawnMenu>() is { IsOpen: true } || _pauseMenuEntity.GetScript<UiPauseMenu>() is { IsOpen: true })
        {
            // Unconditional, not gated on _pendingSource alone (the old check) - a
            // banked Weld/Rope source with Wire's own _pendingSource still empty was
            // silently skipped entirely, leaving THAT pending tint stuck for as long
            // as the menu stayed open. CancelPending() already no-ops per-field on
            // whichever of the three is actually empty, and clears the hover tint too.
            CancelPending();
            InteractTarget = default;
            return;
        }

        TickFlash(deltaTime);
        RefreshInteractTarget();
        HandleModeCycle();
        HandleCancelInput();
        RefreshHoverHighlight();
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

    /// <summary>Direct mode selection - number keys 1-6 map onto the enum in order
    /// (Wire, Light, Colour, Remove, Weld, Rope). Complements the scroll cycle rather
    /// than replacing it: six modes is past where wheel-through-everything stays
    /// usable, and a key select is one press instead of up to five scrolls.</summary>
    private static readonly Key[] ModeSelectKeys =
    {
        Key.Num1, Key.Num2, Key.Num3, Key.Num4, Key.Num5, Key.Num6,
    };

    /// <summary>Mode selection: number keys select directly; the scroll wheel cycles.
    /// Scroll yields to PhysicsGun's own use of the wheel (push/pull distance) while it
    /// has something held - checked by reading its public Held property, not by
    /// contesting the input, since there is no ownership arbitration between two
    /// scripts on the same entity in this engine. The number keys are independent of
    /// the physgun's scroll use, so they work while a prop is held.</summary>
    private void HandleModeCycle()
    {
        for (int i = 0; i < ModeSelectKeys.Length; ++i)
        {
            if (!Input.IsKeyPressed(ModeSelectKeys[i]))
            {
                continue;
            }
            if (i != _modeIndex)
            {
                _modeIndex = i;
                // Same mid-weld hygiene as the wheel path below: a pending source from
                // the mode just left behind must not sit tinted forever. Pressing the
                // CURRENT mode's number is a no-op, not a cancel - re-clicking the same
                // mode's pending source should stay possible.
                CancelPending();
            }
            return;
        }

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
        Vector3 up = CameraBasis.Up(Self);
        Vector3 position = Self.Position + forward * ViewmodelForwardOffset + right * ViewmodelRightOffset + up * ViewmodelUpOffset;
        _viewmodel.SetTransform(position, Self.EulerDegrees, Vector3.One);
    }

    private void TryToolAction()
    {
        switch (Mode)
        {
            case ToolMode.Wire:
                TryWireStep();
                break;
            case ToolMode.Weld:
                TryWeldStep();
                break;
            case ToolMode.Rope:
                TryRopeStep();
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
            Highlight.Clear(_flashTarget, _flashBaseline);
            _flashTarget = default;
        }
    }

    /// <summary>RMB cancels whichever of Wire/Weld/Rope currently has a pending source
    /// banked - the task's own ask for a bound, advertised cancel (see
    /// ModeLabelSuffix's own comment on why Escape is not it). Yields to PhysicsGun's
    /// own RMB-throw exactly the way HandleModeCycle already yields scroll to it while
    /// something is held - same "no cross-script input-ownership concept, check the
    /// other script's own public state instead of contesting the input" idiom, applied
    /// to a second input this file did not previously touch at all.</summary>
    private void HandleCancelInput()
    {
        if (!HasAnyPending())
        {
            return;
        }
        if (GetScript<PhysicsGun>() is { Held.IsValid: true })
        {
            return;
        }
        if (Input.IsMousePressed(MouseButton.Right))
        {
            CancelPending();
        }
    }

    private bool HasAnyPending() => _pendingSource.IsValid || _pendingWeldSource.IsValid || _pendingRopeSource.IsValid;

    /// <summary>Tints whatever is CURRENTLY under the crosshair with
    /// <see cref="ValidTargetTint"/> when <see cref="IsAcceptableNextClick"/> says the
    /// next tool-fire would accept it - "target validity before the click, not a
    /// reject flash after", for whatever is cheap and purely local to decide (see
    /// ValidTargetTint's own field comment on the split with RejectTint). Only
    /// recomputes when the hovered entity actually CHANGES (cached via _hoverTarget),
    /// not every one of 60 frames/second regardless - IsAcceptableNextClick's Wire
    /// branch calls WireHub.WouldCreateCycle, which allocates a HashSet/Stack per
    /// call, so re-running it while aim has not moved at all would be needless
    /// per-frame allocation for no behaviour change.</summary>
    private void RefreshHoverHighlight()
    {
        RaycastHit hit = Aim();
        Entity candidate = hit.DidHit && hit.Entity.IsValid ? hit.Entity : default;
        if (candidate == _hoverTarget)
        {
            return;
        }
        if (_hoverTarget.IsValid)
        {
            Highlight.Clear(_hoverTarget, _hoverBaseline);
        }
        _hoverTarget = candidate;
        _hoverBaseline = default;

        // Never layer a hover tint onto an entity that already carries a STRONGER tint
        // of its own (the banked pending source, or a currently-flashing reject) - the
        // pending tint already means "selected", which implies "would be accepted" for
        // free, and re-tinting a flashing reject target green mid-flash would be a
        // visibly flickering, actively misleading combination.
        if (!candidate.IsValid || candidate == _pendingSource || candidate == _pendingWeldSource || candidate == _pendingRopeSource || candidate == _flashTarget)
        {
            return;
        }
        if (IsAcceptableNextClick(candidate))
        {
            _hoverBaseline = Highlight.Apply(candidate, ValidTargetTint);
        }
    }

    private void ClearHoverHighlight()
    {
        if (_hoverTarget.IsValid)
        {
            Highlight.Clear(_hoverTarget, _hoverBaseline);
        }
        _hoverTarget = default;
        _hoverBaseline = default;
    }

    /// <summary>Whether <paramref name="hit"/> would be accepted as the tool's NEXT
    /// click, reusing the exact same predicates TryWireStep/TryWeldStep/TryRopeStep/
    /// TryRemove already gate their own accept/reject on - one source of truth for
    /// "is this a valid target", read live by both the click handlers and this file's
    /// hover indicator, rather than two copies that could quietly drift apart. Light
    /// and Colour have no reject concept at all (TryAttachLight/TryPaint act on
    /// anything hit), so neither gets a hover indicator - there is nothing to warn
    /// about.</summary>
    private bool IsAcceptableNextClick(Entity hit)
    {
        switch (Mode)
        {
            case ToolMode.Wire:
                if (!_pendingSource.IsValid)
                {
                    return OutputFieldOf(hit) != null;
                }
                if (hit == _pendingSource)
                {
                    return false; // re-hitting the pending source is a CANCEL, not an accept - it already carries its own pending tint
                }
                WireHub? hub = Scene.Find("WireHub").GetScript<WireHub>();
                return hub != null && InputFieldOf(hit, hub) != null && !hub.WouldCreateCycle(_pendingSource, hit);
            case ToolMode.Weld:
                return (!_pendingWeldSource.IsValid || hit != _pendingWeldSource) && HasRigidBody(hit);
            case ToolMode.Rope:
                return (!_pendingRopeSource.IsValid || hit != _pendingRopeSource) && HasRigidBody(hit);
            case ToolMode.Remove:
                return Tags.Has(hit, Tags.Create("grabbable"));
            default:
                return false;
        }
    }

    /// <summary>Begins a pending/flash tint on <paramref name="entity"/>, taking over
    /// any live HOVER tint already on that same entity instead of calling
    /// Highlight.Apply a second time on top of it - a second Apply would read the
    /// hover tint's own already-modified emissive value and wrongly treat THAT as the
    /// "original" to restore later. Returns the baseline the caller stores and later
    /// hands to Highlight.Clear, exactly like a bare Highlight.Apply would.</summary>
    private Vector3 TakeOverHighlight(Entity entity, Vector3 tint)
    {
        if (entity == _hoverTarget)
        {
            Vector3 baseline = _hoverBaseline;
            _hoverTarget = default;
            _hoverBaseline = default;
            if (World.IsValid(entity))
            {
                entity.Material.SetEmissive(tint);
            }
            return baseline;
        }
        return Highlight.Apply(entity, tint);
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
            _pendingBaseline = TakeOverHighlight(_pendingSource, PendingTint);
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
            Highlight.Clear(_pendingSource, _pendingBaseline);
        }
        _pendingSource = default;
        _pendingField = "";

        // Weld/Rope pendings clear with the same keypress (mode switch, spawn menu
        // opening) so a half-finished link never sits tinted forever.
        if (_pendingWeldSource.IsValid)
        {
            Highlight.Clear(_pendingWeldSource, _pendingWeldBaseline);
        }
        _pendingWeldSource = default;
        if (_pendingRopeSource.IsValid)
        {
            Highlight.Clear(_pendingRopeSource, _pendingRopeBaseline);
        }
        _pendingRopeSource = default;
        _pendingRopeAnchorA = default;

        // Force a fresh hover re-evaluation next frame - CancelPending fires on a mode
        // switch too, and last mode's hover-validity verdict for whatever is currently
        // under the crosshair is not necessarily still the right one under the new mode.
        ClearHoverHighlight();
    }

    private void FlashReject(Entity target)
    {
        _flashBaseline = TakeOverHighlight(target, RejectTint);
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

    /// <summary>Applies a pending weld marker's endpoints the first frame its WeldLink
    /// instance actually exists - same late-attach reasoning as PollPendingWire.</summary>
    private void PollPendingWeld()
    {
        if (!_pendingWeldEntity.IsValid)
        {
            return;
        }
        WeldLink? link = _pendingWeldEntity.GetScript<WeldLink>();
        if (link == null)
        {
            return;
        }
        link.Source = _pendingWeldSource;
        link.Target = _pendingWeldTarget;
        _pendingWeldEntity = default;
    }

    /// <summary>Applies a pending rope marker's endpoints and anchors the first frame
    /// its RopeLink instance actually exists - same late-attach reasoning as
    /// PollPendingWire.</summary>
    private void PollPendingRope()
    {
        if (!_pendingRopeEntity.IsValid)
        {
            return;
        }
        RopeLink? link = _pendingRopeEntity.GetScript<RopeLink>();
        if (link == null)
        {
            return;
        }
        link.Source = _pendingRopeSource;
        link.Target = _pendingRopeTarget;
        link.AnchorA = _pendingRopeAnchorA;
        link.AnchorB = _pendingRopeAnchorB;
        link.RestLength = Vector3.Distance(_pendingRopeAnchorA, _pendingRopeAnchorB);
        _pendingRopeEntity = default;
    }

    // ── Weld mode ───────────────────────────────────────────────────────────────

    /// <summary>Two clicks: body A, body B, rigid. Both endpoints must carry a Rigid
    /// Body - a hit without one flash-rejects (welding to a body-less decorative mesh
    /// is meaningless, and a miss is always visible, never silent, in every mode).
    /// Clicking the pending source again cancels, same as Wire.</summary>
    private void TryWeldStep()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }

        if (!_pendingWeldSource.IsValid)
        {
            if (!HasRigidBody(hit.Entity))
            {
                FlashReject(hit.Entity);
                return;
            }
            _pendingWeldSource = hit.Entity;
            _pendingWeldBaseline = TakeOverHighlight(_pendingWeldSource, PendingTint);
            return;
        }

        if (hit.Entity == _pendingWeldSource)
        {
            CancelPending();
            return;
        }
        if (!HasRigidBody(hit.Entity))
        {
            FlashReject(hit.Entity);
            return;
        }

        CreateWeldMarker(_pendingWeldSource, hit.Entity);
        CancelPending();
    }

    // ── Rope mode ───────────────────────────────────────────────────────────────

    /// <summary>Two clicks, each capturing the exact world point under the crosshair:
    /// anchor A on the first body, anchor B on the second. Rest length is the actual
    /// distance between those two points at creation - the rope is born exactly taut.
    /// Same Rigid Body gate and same click-the-source-to-cancel rule as Weld.</summary>
    private void TryRopeStep()
    {
        RaycastHit hit = Aim();
        if (!hit.DidHit || !hit.Entity.IsValid)
        {
            return;
        }

        if (!_pendingRopeSource.IsValid)
        {
            if (!HasRigidBody(hit.Entity))
            {
                FlashReject(hit.Entity);
                return;
            }
            _pendingRopeSource = hit.Entity;
            _pendingRopeAnchorA = hit.Position;
            _pendingRopeBaseline = TakeOverHighlight(_pendingRopeSource, PendingTint);
            return;
        }

        if (hit.Entity == _pendingRopeSource)
        {
            CancelPending();
            return;
        }
        if (!HasRigidBody(hit.Entity))
        {
            FlashReject(hit.Entity);
            return;
        }

        CreateRopeMarker(_pendingRopeSource, _pendingRopeAnchorA, hit.Entity, hit.Position);
        CancelPending();
    }

    private static bool HasRigidBody(Entity e) => e.Component("Rigid Body").Exists;

    private void CreateWeldMarker(Entity source, Entity target)
    {
        // Deliberately NOT MarkTransient() - a weld must survive save/load exactly like
        // a wire does; the marker persists the endpoints and WeldLink's own OnUpdate
        // rebuilds the constraint from them, because handles are meaningless across a
        // reload. Under the non-transient "Welds" container for the same reason the
        // Wires container is non-transient (RuntimeContainers' own file comment on
        // ecs::HasSceneTransientAncestor - a transient parent would silently exclude
        // every marker under it from the save).
        Entity weldEntity = World.Create();
        weldEntity.Name = "Weld";
        weldEntity.AddTransform();
        weldEntity.SetParent(RuntimeContainers.Get("Welds", transient: false));
        weldEntity.AddScript("WeldLink");
        _pendingWeldEntity = weldEntity;
        _pendingWeldSource = source;
        _pendingWeldTarget = target;
    }

    private void CreateRopeMarker(Entity source, Vector3 anchorA, Entity target, Vector3 anchorB)
    {
        // Same persistence shape as CreateWeldMarker, plus the two world anchors and a
        // rest length equal to their current separation - the rope is born exactly
        // taut, never pre-stretched or pre-slack (RopeLink re-derives RestLength from
        // the anchors when its instance appears, so there is exactly one formula).
        Entity ropeEntity = World.Create();
        ropeEntity.Name = "Rope";
        ropeEntity.AddTransform();
        ropeEntity.SetParent(RuntimeContainers.Get("Ropes", transient: false));
        ropeEntity.AddScript("RopeLink");
        _pendingRopeEntity = ropeEntity;
        _pendingRopeSource = source;
        _pendingRopeTarget = target;
        _pendingRopeAnchorA = anchorA;
        _pendingRopeAnchorB = anchorB;
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
