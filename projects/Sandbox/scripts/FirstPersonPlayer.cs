using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The real first-person player, replacing <see cref="DebugFlyCam"/> now that the engine
/// has a Jolt-backed Character Controller. Movement goes through
/// <see cref="CharacterController.Move"/>/<see cref="CharacterController.Jump"/> - never
/// <c>Self.Position</c> - which is exactly what let the player sink into a crate before the
/// solver evicted it, back when this sandbox only had DebugFlyCam's direct transform writes.
///
/// Attached to the 'Player' entity in Sandbox.scene.toml, which also carries the scene's
/// authored [entities.character_controller] block (radius/half_height/step_height/gravity_scale/
/// etc - see that file; gravityScale in particular is tuned there, not here, per that
/// component's own field comment - "a sandbox often wants this above 1" for a less floaty
/// jump, without changing gravity for every rigid body in the scene). The Main Camera is the
/// player's first (and only) child, added as eye height above the player's feet.
///
/// FEET, NOT CENTRE: Jolt's CharacterVirtual convention (unlike every other physics
/// component in this engine) is that the entity's own position IS its feet. The camera
/// child is offset up by <see cref="EyeHeight"/> for exactly that reason - parenting it at
/// (0,0,0) local would put the view at ground level, looking up at everyone's ankles.
///
/// <see cref="Entity.Position"/>/<see cref="Entity.EulerDegrees"/> setters do NOT implement
/// ordinary parent-local composition - see ApplyLook's own comment on why the Player entity's
/// own rotation is deliberately never written. "Yaw turns the player, pitch tilts the camera
/// only" is implemented entirely by tracking yaw/pitch in this script and writing only the
/// camera's world transform every frame. ApplyMovement does NOT recompute a direction from
/// _yaw either - it reads Camera.GetForward/GetRight (the actual rendered look vectors) and
/// flattens them to the horizontal plane, so movement always matches wherever the camera
/// visually points, by construction, rather than by two independent formulas agreeing.
///
/// THREE REAL BUGS LIVE TESTING FOUND HERE, kept as a reminder that matching an existing
/// pattern (DebugFlyCam's) or "it looks right in the code" are not verification - only
/// exercising it is: (1) the Player entity's own rotation used to also be written every
/// frame, which - because Entity transform setters propagate a WORLD delta onto every child
/// (TransformEdit.hpp's ApplyDeltaToSubtree) rather than composing normal parent-local
/// transforms - dragged the camera's rotation by that same delta on top of the camera's own
/// explicit write, and fought the Character Controller's own physics-driven writes to the
/// Player's transform; this surfaced as "opening the spawn menu snaps the camera to a fixed
/// direction" (physics's drag winning once ApplyLook stopped running). (2) Yaw accumulated
/// with the wrong sign (mouse right visibly turned the view left). (3) Movement direction was
/// hand-derived from _yaw with a sin/cos formula that silently did not match the camera's
/// actual rendered forward, so WASD tracked look but never actually aligned with it. Fixed,
/// respectively, by: never writing the Player's own rotation at all; flipping the yaw sign;
/// and reading Camera.GetForward/GetRight directly instead of re-deriving the same vectors by
/// hand.
///
/// MOUSE CAPTURE: Input.CursorLockRequested = true (set every active frame below) asks
/// for real FPS-style pointer lock - cursor hidden, confined to the window, MouseDelta
/// reporting unbounded relative motion instead of an absolute position that stops dead
/// at the screen edge. The engine releases it automatically on losing focus, on Stop, or
/// on Escape (its own hatch back to the editor UI) - re-requesting it every frame is what
/// re-acquires it the moment focus/Play come back, with no state to track here for that.
/// SpawnMenu.Open() sets CursorLockRequested = false (and OsCursorVisible = true) so the
/// cursor is free to click its buttons; closing it lets this resume the request next frame.
///
/// Gated on <see cref="SpawnMenu.IsOpen"/> (via GetScript, not Ui.HasFocus) exactly like
/// DebugFlyCam and PropSpawner: opening the menu suppresses look, movement and jump in the
/// same frame, closing it hands input straight back.
/// </summary>
public sealed class FirstPersonPlayer : EntityScript
{
    // ── Look ─────────────────────────────────────────────────────────────────────
    public float LookSensitivity = 0.15f;

    /// <summary>Flips vertical look. Written by UiHud from SandboxSettings.InvertY -
    /// nothing here reads settings storage directly.</summary>
    public bool InvertY;

    /// <summary>Camera height above the player's feet, in metres. Raised from 1.6
    /// after live feedback that the view sat too low.</summary>
    public float EyeHeight = 1.75f;

    // ── Speed ────────────────────────────────────────────────────────────────────
    /// <summary>Top ground speed while walking, in m/s. A brisk human walk (a real
    /// comfortable pace is closer to 1.4 m/s) traded up for a sandbox that should not
    /// feel like wading through syrup just to reach the props on the far wall.</summary>
    public float WalkSpeed = 3.5f;

    /// <summary>Top ground speed while Shift is held, in m/s. About double the walk
    /// speed - enough to matter when crossing the arena, not so fast that a sprinting
    /// player launches every prop it grazes.</summary>
    public float SprintSpeed = 7.0f;

    // ── Acceleration ─────────────────────────────────────────────────────────────
    /// <summary>Ground speed gained per second while an input direction is held, in
    /// m/s^2. High enough to feel responsive within a couple of frames (this is not
    /// meant to feel sluggish), but not the instant one-frame snap to full speed that
    /// made the old placeholder controller feel like a debug cube instead of a
    /// person.</summary>
    public float GroundAcceleration = 40.0f;

    /// <summary>Ground speed lost per second with no input held, in m/s^2. Higher than
    /// <see cref="GroundAcceleration"/> on purpose - real footing stops a person faster
    /// than it gets them moving, and a controller that coasts to a halt on release
    /// feels loose and unintentional.</summary>
    public float GroundDeceleration = 60.0f;

    /// <summary>Fraction (0..1) of <see cref="GroundAcceleration"/> available to steer
    /// with while airborne. Real air control is limited; this is what turns a jump into
    /// a committed arc - land where you aimed when you left the ground - instead of
    /// letting the player freely helicopter to a new heading mid-air.</summary>
    public float AirControl = 0.25f;

    // ── Jump ─────────────────────────────────────────────────────────────────────
    /// <summary>Vertical launch speed for CharacterController.Jump, in m/s. 4.5 m/s
    /// clears the sandbox's 0.3 m step comfortably with room to spare for a deliberate
    /// hop, without reading as a moon jump - the actual "how floaty does this feel" knob
    /// is the scene's character_controller.gravity_scale, not this number.</summary>
    public float JumpSpeed = 4.5f;

    private Entity _camera;
    private Entity _pauseMenuEntity;
    private float _yaw;
    private float _pitch;

    /// <summary>Current horizontal (Y always 0) ground-relative velocity this script is
    /// easing toward the input's target every frame. The Character Controller itself
    /// owns the vertical component (gravity/ground-follow/jump) - see Move's own doc
    /// comment - so this is the whole of what acceleration/deceleration smooths.</summary>
    private Vector3 _horizontalVelocity;

    public override void OnAttach()
    {
        _camera = Self.ChildCount > 0 ? Self.GetChild(0) : default;
        if (!_camera.IsValid)
        {
            Log.Warn("[Sandbox] FirstPersonPlayer: no camera child on the Player entity; look will do nothing.");
        }
        _pauseMenuEntity = Scene.Find("NetSession");

        // The scene-placed solo 'Player' entity carries no NetPlayerRig (that script
        // is for the networked prefab only - see its own file comment), so nothing
        // else ever attaches PlayerBody there. NetPlayerRig.OnAttach already attaches
        // it for every networked player copy (owner and remote alike) well before this
        // runs, so this guard is a no-op in that case and the only real spawn path for
        // the solo entity.
        if (Self.GetScript<PlayerBody>() == null)
        {
            Self.AddScript(nameof(PlayerBody));
        }

        Vector3 euler = Self.EulerDegrees;
        _yaw = euler.Y;
        _pitch = 0.0f;
    }

    public override void OnUpdate(float deltaTime)
    {
        // Owner-only: a remote peer's copy of this player must never read local input
        // or move. Self IS the Player entity here (unlike the camera-attached scripts),
        // and the prefab's root carries the NetworkIdentity, so Net.HasAuthority(Self)
        // is correct directly - no ResolveOwnershipEntity indirection needed. True
        // offline, so single-player is unaffected - verified against
        // aether_net_has_authority's own offline short-circuit in NetExports.cpp.
        if (!Net.HasAuthority(Self))
        {
            return;
        }

        // GetScript<T>() (inherited, bare) looks up scripts on THIS script's own Self -
        // the Player entity. SpawnMenu and PhysicsGun both live on the Main Camera child
        // instead, so the lookup has to go through the cached _camera entity handle
        // (Entity itself exposes the same GetScript<T>() for exactly this cross-entity
        // case) - a bare GetScript<SpawnMenu>() here would silently always return null
        // and this gate would never actually fire. Caught by tracing the API, not by a
        // test: nothing here would visibly break until someone opened the menu while
        // trying to walk and wondered why the player kept moving under it.
        if (_camera.GetScript<SpawnMenu>() is { IsOpen: true })
        {
            return;
        }
        if (_pauseMenuEntity.GetScript<UiPauseMenu>() is { IsOpen: true })
        {
            return;
        }

        // Real FPS pointer lock while actually playing - see this class's own file
        // comment. Re-requesting every frame means Escape/focus-loss/Stop release it and
        // it simply comes back the next frame this runs (e.g. clicking back into the
        // window), with no extra state to track here.
        Input.CursorLockRequested = true;

        // While the gun is rotating a held prop (E held), mouse movement spins the prop
        // instead of turning the player - see PhysicsGun's own file comment. Movement
        // still works while rotating (GMod lets you walk and orient a held prop at the
        // same time); only look is redirected.
        if (_camera.GetScript<PhysicsGun>() is not { IsRotatingProp: true })
        {
            ApplyLook();
        }
        ApplyMovement(deltaTime);
    }

    private void ApplyLook()
    {
        // Real per-frame relative delta from the engine's pointer lock (see this class's
        // own file comment) - unbounded, not an absolute position that stops at the
        // screen edge.
        Vector2 delta = Input.MouseDelta;

        // Sign confirmed by live testing, not assumed: an earlier version used
        // "+= delta.X" (matching DebugFlyCam) and mouse-right visibly turned the view
        // LEFT. DebugFlyCam has apparently never actually been played interactively
        // either - copying its formula was not verification. This is now the one place
        // yaw sign is decided; ApplyMovement below no longer re-derives a direction from
        // _yaw at all (see its own comment), so this sign only ever affects look.
        _yaw -= delta.X * LookSensitivity;
        float pitchSign = InvertY ? 1.0f : -1.0f;
        _pitch = Math.Clamp(_pitch + delta.Y * LookSensitivity * pitchSign, -89.0f, 89.0f);

        // Deliberately never writes Self.EulerDegrees. ecs::SetWorldTransform (what every
        // Entity.Position/EulerDegrees setter goes through) does not implement ordinary
        // parent-local composition - it computes a WORLD delta from the entity's old to
        // new transform and re-applies that same delta to every child's current world
        // transform (TransformEdit.hpp's own ApplyDeltaToSubtree). Rotating the Player
        // therefore drags the camera child's rotation by that delta too, on top of the
        // camera's own explicit write two lines below - and the Character Controller's
        // physics sync writes the Player's transform on its own schedule (translation
        // only, but still a SetWorldTransform call), dragging the camera again independent
        // of this script's frame. This drag conflict was the menu-opens-snaps-the-camera
        // symptom; the yaw sign above was a second, separate bug live testing also found.
        // A visible body mesh exists now (see PlayerBody), but it is a CHILD entity
        // with its own independently-written world rotation (driven off _yaw, same as
        // the camera two lines below) - never a read of Self.EulerDegrees. Leaving the
        // Player entity's own transform rotation untouched still costs nothing and
        // still removes the drag conflict entirely.
        if (_camera.IsValid)
        {
            _camera.EulerDegrees = new Vector3(_pitch, _yaw, 0.0f);
            _camera.Position = Self.Position + new Vector3(0.0f, EyeHeight, 0.0f);
        }
    }

    private void ApplyMovement(float deltaTime)
    {
        // Yaw-only basis: pitch (looking up/down) must never change how fast or which
        // way the player walks. Derived from the CAMERA's actual forward/right (real
        // engine-computed vectors, via the exact same ForwardOf/RightOf the renderer
        // uses - see Camera.GetForward's own native implementation), flattened to the
        // horizontal plane, rather than recomputed by hand from _yaw: an earlier version
        // hand-rolled sin/cos off _yaw and it silently diverged from where the camera
        // actually rendered - live testing found WASD moving in a direction that changed
        // with look but never actually matched it. Asking the engine what the camera
        // really faces removes that whole class of convention-mismatch bug by
        // construction, instead of re-deriving the same answer a second, fallible way.
        Vector3 forward = FlattenToGround(Camera.GetForward(_camera));
        Vector3 right = FlattenToGround(Camera.GetRight(_camera));

        float x = Input.GetAxisRaw(Key.A, Key.D);
        float z = Input.GetAxisRaw(Key.S, Key.W);
        Vector3 inputDir = forward * z + right * x;
        bool hasInput = inputDir.LengthSquared() > 0.0001f;
        if (hasInput)
        {
            inputDir = Vector3.Normalize(inputDir);
        }

        bool grounded = CharacterController.IsGrounded(Self);
        float topSpeed = Input.IsKeyDown(Key.LeftShift) ? SprintSpeed : WalkSpeed;
        Vector3 targetVelocity = inputDir * topSpeed;

        // Ground: fast, and different rates for speeding up vs. coasting to a stop.
        // Air: both directions throttled by the same AirControl fraction - a jump
        // commits to roughly the arc it was launched on rather than being freely
        // steerable, but still lets a player nudge their landing.
        float accel = grounded
                ? (hasInput ? GroundAcceleration : GroundDeceleration)
                : GroundAcceleration * AirControl;

        _horizontalVelocity = MoveTowards(_horizontalVelocity, targetVelocity, accel * deltaTime);

        float speed = _horizontalVelocity.Length();
        Vector3 moveDirection = speed > 0.0001f ? _horizontalVelocity / speed : Vector3.Zero;

        // Always called, even at zero speed: Move's velocity persists until the next
        // call (see its own doc comment), so skipping this when there is no input would
        // leave the player sliding forever on whatever it last pressed instead of
        // easing to a stop under GroundDeceleration.
        CharacterController.Move(Self, moveDirection, speed);

        // Reuses the SAME speed/yaw already computed above for Move/ApplyLook, rather
        // than PlayerBody re-deriving them from position deltas (see that class's own
        // file comment on why this owner-driven signal beats its position-delta
        // fallback: no physics-jitter false positives, and yaw matches the camera the
        // owner is actually looking through).
        Self.GetScript<PlayerBody>()?.SetLocomotionState(_yaw, speed);

        if (Input.IsKeyPressed(Key.Space) && grounded)
        {
            CharacterController.Jump(Self, JumpSpeed);
        }
    }

    /// <summary>Moves <paramref name="current"/> toward <paramref name="target"/> by at
    /// most <paramref name="maxDelta"/>, without ever overshooting it - the vector
    /// equivalent of Unity's Mathf.MoveTowards, which System.Numerics does not ship.</summary>
    private static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDelta)
    {
        Vector3 toTarget = target - current;
        float distance = toTarget.Length();
        if (distance <= maxDelta || distance < 1e-5f)
        {
            return target;
        }
        return current + toTarget / distance * maxDelta;
    }

    /// <summary>Zeroes the Y component and renormalizes - turns a real (possibly
    /// pitched) look vector into a pure horizontal direction, which is what keeps
    /// looking up/down from changing movement speed or direction (see ApplyMovement's
    /// own comment). Returns Vector3.Zero for a vector that was already vertical
    /// (looking straight up/down), rather than an undefined/NaN direction.</summary>
    private static Vector3 FlattenToGround(Vector3 v)
    {
        Vector3 flat = new Vector3(v.X, 0.0f, v.Z);
        float lengthSq = flat.LengthSquared();
        return lengthSq > 0.0001f ? flat / MathF.Sqrt(lengthSq) : Vector3.Zero;
    }
}
