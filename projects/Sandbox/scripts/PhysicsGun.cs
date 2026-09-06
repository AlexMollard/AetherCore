using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// GMod's physgun: HOLDING left mouse fires a beam every frame; the instant its raycast
/// slides onto a grabbable prop it grabs (no separate click needed once aimed), and
/// floats the prop in front of the camera. Releasing drops it. Right mouse throws it,
/// scroll pushes/pulls it, E-plus-mouse spins it. Attached to the
/// same Main Camera entity as <see cref="PropSpawner"/>/<see cref="SpawnMenu"/>, so
/// <c>Self</c> is the camera and <c>Camera.GetForward(Self)</c>/<c>Self.Position</c> are the
/// aim ray and hold-point origin with no extra indirection.
///
/// HELD PROPS STAY REAL PHYSICS OBJECTS - driven by velocity, never teleported by writing a
/// transform. DriveHeldProp is a critically-damped spring on velocity: every frame it
/// recomputes the target point, the position error, and a "desired" velocity proportional to
/// that error (<see cref="HoldStiffness"/>, the spring's P term), then eases the prop's actual
/// velocity toward that desired value (<see cref="HoldDamping"/>, the D term) instead of
/// snapping to it - the damping is what makes it settle instead of oscillating forever, and
/// because it is still a real velocity, the held prop keeps colliding with walls and other
/// props on the way to its target rather than tunnelling through them.
///
/// ENGINE WORK THAT LANDED WHILE THIS WAS BEING WRITTEN - used directly, not deferred:
/// Physics.SetGravityFactor and Physics.AddImpulseAtPoint both shipped mid-task (confirmed by
/// reading Physics.cs directly, not by trusting the task description), so this uses them as
/// intended: SetGravityFactor(held, 0) on grab is the actual "weightless while held" instead of
/// a script-side compensation hack, and AddImpulseAtPoint (impulse off the centre of mass) is
/// the actual throw primitive instead of a plain AddImpulse that would only ever slide a prop
/// flat. Also landed: SetLinearVelocity no longer drops a velocity set the same frame a body
/// is created (see PropSpawner's own file comment) - irrelevant to grabbing an
/// already-existing arena prop (its body was already baked long before this script ever
/// touches it), but worth knowing the class of bug is fixed upstream now, not worked around.
///
/// GRABBABLE FILTERING: there is no script-side "is this body dynamic" query, so grabbability
/// is a shared "grabbable" tag instead - OnAttach tags every child of the scene's "Props"
/// group once, and PropSpawner tags each prop it spawns at creation (see its own file
/// comment). A raycast hit is only grabbable if it carries that tag, which is what keeps
/// walls/ground/step/the player itself un-grabbable without the gun needing to know their
/// names or types.
///
/// SELF-HIT GUARD: the Character Controller now has a Jolt inner body so raycasts see
/// characters too (see CharacterControllerComponent's own file comment - this is exactly what
/// makes a physgun able to grab or hit a player later). The eye position sits inside that
/// capsule's own extent, so the aim ray starts <see cref="RayStartOffset"/> in front of the
/// camera rather than exactly at it, and any hit against the player entity itself is rejected
/// as a second guard.
///
/// OWNERSHIP: grabbing is split into "find a candidate" (TryGrab's raycast + tag check)
/// and "claim it" (<see cref="TryClaim"/>, which sends Net.RequestOwnership and nothing
/// more). Offline, or reclaiming a prop already ours, the grant is immediate and the
/// hold starts the same frame it always did. Otherwise the request goes to the host and
/// the answer is not in yet - the candidate sits in <see cref="_pendingClaim"/>, driven
/// by neither the spring nor gravity-zero (a non-owned body is replication's to move,
/// not ours), until <see cref="PollPendingClaim"/> notices Net.IsOwner flip true, or
/// gives up after <see cref="ClaimTimeoutSeconds"/> if the host silently refuses. Release
/// hands ownership back to the host so a dropped prop is claimable again instead of
/// staying locked to whoever last held it.
///
/// <see cref="_held"/> and every other piece of hold state are plain instance fields, not
/// statics, for the same reason PropSpawner's own file comment gives: a static registry would
/// need cross-peer reconciliation, an instance field is naturally per-authority.
///
/// Gated on <see cref="SpawnMenu.IsOpen"/> exactly like every other gameplay script here -
/// opening the menu while holding something releases it cleanly (restores gravity and the
/// held tint) rather than leaving a weightless prop frozen in mid-air forever.
///
/// AUTHORITY: OnUpdate's entire body is gated on <see cref="Net.HasAuthority"/> of the
/// PLAYER (<see cref="_player"/>), not Self - a remote peer's copy of this script must
/// never read local input, grab a prop, or fire. Intended containment is NetPlayerRig
/// only ever attaching this script to the owning peer's copy at all; this gate is the
/// second, independent layer for anyone who bakes it onto a shared prefab directly. The
/// crosshair is built lazily on the first authorized OnUpdate tick rather than in
/// OnAttach - see <see cref="EnsureHud"/> for why OnAttach itself is the wrong place to
/// check authority.
/// </summary>
public sealed class PhysicsGun : EntityScript
{
    // ── Grab / range ─────────────────────────────────────────────────────────────
    /// <summary>How far the aim ray reaches to find something to grab, in metres. Long
    /// enough to grab across most of the sandbox arena (24 m wide) without walking up to
    /// everything first.</summary>
    public float MaxGrabDistance = 10.0f;

    /// <summary>Nearest the hold point can be pulled to the camera, in metres. Keeps a
    /// large held prop from clipping into the player's own capsule (0.3 m radius) when
    /// scrolled all the way in.</summary>
    public float MinHoldDistance = 1.5f;

    /// <summary>Farthest the hold point can be pushed with scroll, in metres. Short of
    /// the arena's own half-width so a pushed prop stays reachable instead of ending up
    /// parked against - or past - a wall.</summary>
    public float MaxHoldDistance = 8.0f;

    /// <summary>Metres of hold distance per unit of scroll delta. A few notches covers a
    /// useful push/pull range without a single flick sending the prop from your face to
    /// the far wall.</summary>
    public float ScrollStep = 0.6f;

    /// <summary>How far in front of the camera the aim ray actually starts, in metres.
    /// See this class's own file comment on the Character Controller's inner body - eye
    /// height sits inside the player's own capsule, so starting the ray exactly at the
    /// camera risks an immediate self-hit.</summary>
    public float RayStartOffset = 0.35f;

    // ── Hold spring (critically-damped velocity controller) ─────────────────────
    /// <summary>Spring "P" term, 1/s: how much of the current position error becomes
    /// desired velocity. Strong enough that a held prop tracks the camera briskly across
    /// the arena, not so strong it violently overshoots a sudden turn.</summary>
    public float HoldStiffness = 45.0f;

    /// <summary>Spring "D" term, 1/s: how fast the prop's actual velocity eases toward
    /// the spring's desired value instead of snapping to it every frame. This is what
    /// makes the hold settle to a stop instead of oscillating around the target
    /// forever.</summary>
    public float HoldDamping = 14.0f;

    /// <summary>Hard clamp on the spring's commanded speed, in m/s. Without this, a prop
    /// that got wedged behind geometry and then broke free would be whipped toward the
    /// target at whatever speed the accumulated error implied.</summary>
    public float MaxHoldSpeed = 22.0f;

    // ── Rotate (E + mouse) ────────────────────────────────────────────────────────
    /// <summary>Angular velocity (degrees/s) imparted per pixel of mouse delta while E is
    /// held. Tuned so a full mouse-width drag is a couple of full spins - enough to
    /// orient a prop deliberately, not so twitchy that a small hand movement sends it
    /// spinning out of control.</summary>
    public float RotateSensitivity = 0.9f;

    // ── Throw / held look ────────────────────────────────────────────────────────
    /// <summary>Throw impulse magnitude, in newton-seconds, along the camera's forward
    /// direction. Physics.AddImpulseAtPoint divides by mass like any impulse (see its own
    /// doc comment), so a light Marble (2 kg) flies much farther than a Heavy Ball
    /// (60 kg) for the same click - which is correct, not a bug: the same swing throws a
    /// marble farther than a bowling ball.</summary>
    public float ThrowImpulseStrength = 26.0f;

    /// <summary>How far off the prop's centre of mass the throw impulse lands, in
    /// metres, along the camera's right axis. Off-centre is the whole point -
    /// AddImpulseAtPoint at dead centre would impart no spin at all, and a thrown prop
    /// that only ever slides flat looks broken.</summary>
    public float ThrowOffCenter = 0.15f;

    /// <summary>Emissive colour tint applied to a held prop and cleared on release - the
    /// task's own minimum bar for "some visual indication of what is currently held".
    /// Assumes every grabbable prop is authored at emissive black (true for every prop in
    /// this sandbox, scene-authored or spawned), so clearing to Vector3.Zero on release is
    /// safe; a prop with its own baseline emissive would need that value remembered
    /// instead of assumed.</summary>
    public Vector3 HeldEmissiveTint = new(0.15f, 0.55f, 0.85f);

    /// <summary>Crosshair dot size in pixels. Small and unobtrusive - it only needs to
    /// mark screen centre, not draw attention to itself.</summary>
    public float CrosshairSize = 6.0f;

    /// <summary>True while E is held and something is held - <see cref="FirstPersonPlayer"/>
    /// reads this (cross-entity, via its cached camera reference) to redirect mouse
    /// movement into prop rotation instead of player look for exactly as long as this is
    /// true.</summary>
    public bool IsRotatingProp { get; private set; }

    /// <summary>The prop currently held, or an invalid entity - read by UiHud for the
    /// "Holding: &lt;name&gt;" label. PhysicsGun keeps driving/tinting it itself; this is a
    /// read-only window onto the same _held field, not a second way to set it.</summary>
    public Entity Held => _held;

    /// <summary>The crosshair's own canvas - UiHud parents its "Holding" label and F/Q
    /// hint row onto this SAME canvas rather than building a second one, so this
    /// script's own menuOpen suppression (below) already hides them for free.</summary>
    public Entity Hud => _hud;

    private Entity _player;
    private Entity _hud;
    private Entity _held;
    private Entity _pauseMenuEntity;
    private Beam? _beam;

    /// <summary>Current hold distance in front of the camera, adjusted by scroll and
    /// re-seeded on every new grab from how far away the prop actually was.</summary>
    private float _holdDistance;

    /// <summary>Seconds a claim may sit unanswered by the host before TryGrab gives up
    /// on it. Covers both a silent refusal (someone else already owns the prop - see
    /// TryClaim's own doc comment) and a request that never arrives at all, so a denied
    /// or lost claim cannot leave the gun waiting forever on nothing.</summary>
    public float ClaimTimeoutSeconds = 1.5f;

    /// <summary>The candidate a claim was sent for but not yet granted (client only -
    /// see TryGrab). Not driven, not tinted, not gravity-zeroed: until Net.IsOwner
    /// reports true, this prop is still the replication system's to move, not ours.</summary>
    private Entity _pendingClaim;
    private float _pendingClaimElapsed;

    public override void OnAttach()
    {
        _player = Self.Parent;
        _pauseMenuEntity = Scene.Find("NetSession");

        TagId grabbable = Tags.Create("grabbable");
        Entity props = Scene.Find("Props");
        if (props.IsValid)
        {
            for (int i = 0; i < props.ChildCount; ++i)
            {
                Tags.Add(props.GetChild(i), grabbable);
            }
        }
        else
        {
            Log.Warn("[Sandbox] PhysicsGun: scene has no 'Props' entity; only props spawned at runtime will be grabbable.");
        }
    }

    public override void OnDetach()
    {
        _beam?.Destroy();
        _beam = null;
    }

    /// <summary>Builds the crosshair on this peer's first authorized frame rather than
    /// in OnAttach - HasAuthority is not knowable yet when OnAttach runs on a client
    /// (see NetPlayerRig/OnOwnershipChanged's own remarks: the owning connection id can
    /// arrive a frame or more later), and OnAttach never runs twice, so checking
    /// authority there would permanently skip this UI for the real owner too. Guards
    /// the same duplicate-canvas class of bug SpawnMenu's own file comment covers, for
    /// anyone who bakes this script onto a shared prefab directly instead of going
    /// through the rig.</summary>
    private void EnsureHud()
    {
        if (_hud.IsValid)
        {
            return;
        }
        _hud = Ui.CreateCanvas();
        _hud.MarkTransient(); // runtime UI, never save-worthy - confirmed live: an accidental save during Play previously baked a duplicate crosshair canvas permanently into Sandbox.scene.toml
        Entity crosshair = Ui.CreateImage(_hud);
        Ui.SetAnchors(crosshair, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(crosshair, new Vector2(0.5f, 0.5f));
        Ui.SetRect(crosshair, 0.0f, 0.0f, CrosshairSize, CrosshairSize);
        Ui.SetImageColor(crosshair, new Vector4(1.0f, 1.0f, 1.0f, 0.85f));
    }

    public override void OnUpdate(float deltaTime)
    {
        // Owner-only: input, grabbing, holding and throwing must never run for a
        // remote copy of another peer's player. _player (not Self, the camera) is the
        // check because Self carries no NetworkIdentity of its own - NetworkContext's
        // ownership resolver only redirects a ragdoll bone to its root, not an
        // arbitrary child, so Net.HasAuthority(Self) here would silently read true
        // unconditionally (an entity with no NetworkIdentity reads as "mine") and gate
        // nothing at all. True offline, so single-player is unaffected - verified
        // against aether_net_has_authority/aether_net_is_owner's own offline
        // short-circuit in NetExports.cpp, not assumed.
        if (!Net.HasAuthority(_player))
        {
            return;
        }
        EnsureHud();

        bool menuOpen = GetScript<SpawnMenu>() is { IsOpen: true }
                || _pauseMenuEntity.GetScript<UiPauseMenu>() is { IsOpen: true };
        _hud.SetActive(!menuOpen); // no crosshair floating over the spawn menu
        if (menuOpen)
        {
            if (_held.IsValid)
            {
                Release();
            }
            _pendingClaim = default;
            _pendingClaimElapsed = 0.0f;
            _beam?.Hide();
            return;
        }

        if (!World.IsValid(_held))
        {
            _held = default; // clears a stale handle so Held (read by UiHud) never leaks a dead entity

            // HOLD semantics, not click: fire is Input.IsMouseDown, checked every frame,
            // not IsMousePressed - the user's own ask ("fire the laser on hold and slide
            // across an object to then grab it"). Releasing the button while a claim is
            // still in flight abandons it - holding the trigger IS the grab attempt, so
            // letting go mid-request should not still land a grab a moment later.
            if (!Input.IsMouseDown(MouseButton.Left))
            {
                _beam?.Hide();
                _pendingClaim = default;
                _pendingClaimElapsed = 0.0f;
                return;
            }

            if (_pendingClaim.IsValid)
            {
                PollPendingClaim(deltaTime);
            }
            else
            {
                // Grab happens on CONTACT, not on press: the beam is already firing
                // every frame the button is down, and the instant its raycast slides
                // onto a grabbable prop, TryGrab claims it right then - the player never
                // has to release and re-press while already aimed at it.
                TryGrab(deltaTime);
            }
            return;
        }

        if (Input.IsMousePressed(MouseButton.Right))
        {
            Throw();
            return;
        }
        if (!Input.IsMouseDown(MouseButton.Left))
        {
            Release();
            return;
        }

        _holdDistance = Math.Clamp(_holdDistance + Input.ScrollDelta.Y * ScrollStep, MinHoldDistance, MaxHoldDistance);

        if (Input.IsKeyDown(Key.E))
        {
            IsRotatingProp = true;
            Vector2 delta = Input.MouseDelta;
            Vector3 angularVelocity = Vector3.UnitY * (delta.X * RotateSensitivity)
                    + Camera.GetRight(Self) * (delta.Y * RotateSensitivity);
            Physics.SetAngularVelocity(_held, angularVelocity);
        }
        else
        {
            IsRotatingProp = false;
            // Don't leave it spinning the instant E is released.
            Physics.SetAngularVelocity(_held, Vector3.Zero);
        }

        DriveHeldProp(deltaTime);
        // Beam follows the held prop itself (the Entity-target overload re-reads its
        // live position every call), not the aim ray - once grabbed, the laser is
        // "attached to what you're holding", not "still searching".
        Beam beam = EnsureBeam();
        beam.Show(Self.Position + Camera.GetForward(Self) * RayStartOffset, _held, deltaTime);
    }

    private void DriveHeldProp(float deltaTime)
    {
        Vector3 targetPoint = Self.Position + Camera.GetForward(Self) * _holdDistance;
        Vector3 currentPosition = Physics.GetPosition(_held);
        Vector3 error = targetPoint - currentPosition;

        Vector3 currentVelocity = Physics.GetLinearVelocity(_held);
        Vector3 desiredVelocity = error * HoldStiffness;
        Vector3 newVelocity = Vector3.Lerp(currentVelocity, desiredVelocity, Math.Clamp(HoldDamping * deltaTime, 0.0f, 1.0f));

        float speed = newVelocity.Length();
        if (speed > MaxHoldSpeed)
        {
            newVelocity = newVelocity / speed * MaxHoldSpeed;
        }

        Physics.SetLinearVelocity(_held, newVelocity);
    }

    private Beam EnsureBeam() => _beam ??= Beam.Create("PhysicsGun Beam");

    /// <summary>Casts every frame the trigger is held (see OnUpdate's own comment on why
    /// this is not IsMousePressed) and draws the beam from muzzle to whatever it is
    /// currently aimed at - a fixed point past MaxGrabDistance when nothing is hit, the
    /// hit point when something is, grabbable or not (so aiming at a wall still shows
    /// the beam stopping there, not passing through). Only actually GRABS when the hit
    /// is tagged "grabbable" - the beam sliding across a wall or the player's own
    /// capsule never claims anything.</summary>
    private void TryGrab(float deltaTime)
    {
        Vector3 forward = Camera.GetForward(Self);
        Vector3 origin = Self.Position + forward * RayStartOffset;
        RaycastHit hit = Physics.Raycast(origin, forward, MaxGrabDistance);

        Beam beam = EnsureBeam();
        Vector3 beamEnd = hit.DidHit ? hit.Position : origin + forward * MaxGrabDistance;
        beam.Show(origin, beamEnd, deltaTime);

        if (!hit.DidHit || !hit.Entity.IsValid || hit.Entity == _player)
        {
            return;
        }
        if (!Tags.Has(hit.Entity, Tags.Create("grabbable")))
        {
            return;
        }
        if (!TryClaim(hit.Entity))
        {
            return;
        }

        if (Net.IsOwner(hit.Entity))
        {
            // Offline, or already ours: Net.RequestOwnership's own doc comment says the
            // change is immediate in both cases, so the hold starts this frame exactly
            // like it did before ownership existed at all - single-player is unaffected.
            BeginHold(hit.Entity);
        }
        else
        {
            // Sent to the host; the answer is not in yet. Do not zero gravity, tint, or
            // drive velocity on a prop we don't own - the replication system is still
            // authoritative over it (forces it kinematic) until the grant actually lands.
            _pendingClaim = hit.Entity;
            _pendingClaimElapsed = 0.0f;
        }
    }

    /// <summary>Polls a sent-but-unanswered claim once per frame while nothing is held.
    /// Starts the hold the instant Net.IsOwner flips true, and gives up cleanly (see
    /// ClaimTimeoutSeconds) if the host silently refuses or the candidate is destroyed
    /// while waiting.</summary>
    private void PollPendingClaim(float deltaTime)
    {
        if (!World.IsValid(_pendingClaim))
        {
            _pendingClaim = default;
            _pendingClaimElapsed = 0.0f;
            return;
        }
        if (Net.IsOwner(_pendingClaim))
        {
            BeginHold(_pendingClaim);
            return;
        }
        _pendingClaimElapsed += deltaTime;
        if (_pendingClaimElapsed >= ClaimTimeoutSeconds)
        {
            _pendingClaim = default;
            _pendingClaimElapsed = 0.0f;
        }
    }

    /// <summary>Starts actually driving <paramref name="prop"/> once ownership is
    /// confirmed ours - shared by the immediate-grant path in TryGrab and the
    /// delayed-grant path in PollPendingClaim. Hold distance is measured from the
    /// prop's CURRENT position rather than the original raycast hit point, since a
    /// delayed grant may land several frames after the camera moved.</summary>
    private void BeginHold(Entity prop)
    {
        _held = prop;
        _pendingClaim = default;
        _pendingClaimElapsed = 0.0f;
        _holdDistance = Math.Clamp(Vector3.Distance(Self.Position, Physics.GetPosition(prop)), MinHoldDistance, MaxHoldDistance);

        // Wake a sleeping prop the instant it is grabbed (see Physics.SetBodyActive's own
        // doc comment - a sleeping body otherwise ignores velocity writes until something
        // else wakes it), then make it weightless for the duration of the hold.
        Physics.SetBodyActive(_held, true);
        Physics.SetGravityFactor(_held, 0.0f);
        _held.Material.SetEmissive(HeldEmissiveTint);
    }

    /// <summary>
    /// The ownership boundary: sends the claim request and nothing more - it does NOT
    /// mean the prop is ours (see Net.RequestOwnership's own doc comment: the host may
    /// still silently refuse a request for something another connected player already
    /// holds). TryGrab checks Net.IsOwner right after this returns to tell an immediate
    /// grant (offline, or already ours) from one still in flight, and PollPendingClaim
    /// is what actually notices a delayed grant or gives up on a refused/lost one.
    /// Returns false only when <paramref name="prop"/> is not a live handle.
    /// </summary>
    private static bool TryClaim(Entity prop) => Net.RequestOwnership(prop);

    private void Release()
    {
        if (World.IsValid(_held))
        {
            Physics.SetGravityFactor(_held, 1.0f);
            Physics.SetAngularVelocity(_held, Vector3.Zero);
            _held.Material.SetEmissive(Vector3.Zero);
            // Hand ownership back to the host so someone else (or this player, later)
            // can claim it - a dropped prop must not stay locked to whoever last held it.
            Net.ReleaseOwnership(_held);
        }
        _held = default;
        _pendingClaim = default;
        _pendingClaimElapsed = 0.0f;
        IsRotatingProp = false;
    }

    private void Throw()
    {
        if (!World.IsValid(_held))
        {
            return;
        }
        Vector3 forward = Camera.GetForward(Self);
        Vector3 offCentrePoint = Physics.GetPosition(_held) + Camera.GetRight(Self) * ThrowOffCenter;
        Physics.AddImpulseAtPoint(_held, forward * ThrowImpulseStrength, offCentrePoint);
        Release();
    }
}
