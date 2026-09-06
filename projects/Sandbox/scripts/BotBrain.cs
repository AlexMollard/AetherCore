using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// An AI player: a small, legible state machine that wanders the arena, picks a
/// grabbable prop, walks to it, grabs it with the same physics primitives
/// <see cref="PhysicsGun"/> uses (via <see cref="BotGrabber"/>), carries it a while,
/// then flings or drops it and repeats - forever, with nobody at the keyboard. Its
/// purpose is as much a test harness as a feature: it exists to keep exercising
/// movement, grabbing and (later) multiplayer replication without a human driving.
///
/// MOVEMENT IS THE CHARACTER CONTROLLER, NEVER A TRANSFORM WRITE: every locomotion
/// call in this file goes through <see cref="CharacterController.Move"/>, exactly
/// like <see cref="FirstPersonPlayer"/> - see that class's own file comment on why
/// writing <c>Self.Position</c> directly is the one thing never to do here. This
/// entity's own position is its FEET (the Character Controller's Jolt convention,
/// unlike every rigid-body prop in this scene) - <see cref="EyeHeight"/> is added
/// wherever this script needs an eye-level point (aim origin, hold target), the
/// same role <c>FirstPersonPlayer.EyeHeight</c> plays for the human.
///
/// NO PATHFINDING: there is no navmesh or path planner here, and none is added.
/// Every steering decision below is "face the target, walk straight at it"; when
/// that is not good enough (something is in the way, the target is unreachable),
/// the state machine's job is to notice and give up, not to route around the
/// obstacle. See <see cref="TickApproach"/> and the stuck watchdog in
/// <see cref="UpdateStuckWatchdog"/> for where that giving-up actually happens.
///
/// UNWEDGEABLE BY CONSTRUCTION: every state exits either on a DISTANCE condition
/// (arrived, in grab range) or a fixed TIMER (ApproachTimeout, CarryDuration,
/// RecoverBackupDuration, MaxGrabMisses), and the stuck watchdog
/// (<see cref="UpdateStuckWatchdog"/>) forces a transition out of Wander/Approach/
/// Carry whenever the bot has not made horizontal progress for
/// <see cref="StuckCheckInterval"/> seconds, regardless of which of those states it
/// is in or why it stalled. There is no state here that waits forever on a
/// condition that might never become true - Recover itself, the state every failure
/// funnels into, exits purely on a timer (<see cref="RecoverBackupDuration"/>), so it
/// cannot itself wedge even if backing up does not actually clear the obstruction.
/// A bot that silently sits in a corner forever would be worse than no bot at all
/// (it would look like coverage that is not there), so every failure path above
/// logs via <see cref="Log.Warn"/> - check the console log, not just the screen, to
/// see it recovering.
///
/// DECISIONS SEPARABLE FROM ACTUATION: this class only ever mutates the world
/// through two surfaces - <see cref="CharacterController.Move"/> for locomotion, and
/// the owned <see cref="_grabber"/> (<see cref="BotGrabber"/>) for grab/hold/throw.
/// Everything else in this file (the state enum, the per-state Tick methods, target
/// selection, the stuck watchdog) is pure decision-making: it reads positions and
/// tags but does not touch physics. A networked version of this bot would run this
/// entire OnUpdate only on the authority (the same connection FirstPersonPlayer's
/// movement would run on) and have every peer replicate the resulting transform/held
/// state the normal way, exactly like any other networked character - no networking
/// code is added here per this task's own instruction, but the seam is exactly this:
/// gate the whole decision phase, not each individual call within it.
///
/// PER-BOT INSTANCE STATE, NEVER STATIC: every field below is an instance field, for
/// the same reason PropSpawner's and PhysicsGun's own file comments give - a static
/// registry would need cross-peer reconciliation once this is networked; an instance
/// field is naturally per-authority and needs nothing extra.
///
/// MULTIPLE BOTS, NO SHARED STATE: nothing here assumes it is the only bot in the
/// scene. Each instance tags its own <see cref="Self"/> "bot" (OnAttach) purely so
/// <see cref="BotSoakMonitor"/> can find every bot without a registry; the actual
/// per-bot decision state stays exactly as private/instance as ever. Two bots CAN
/// both successfully "grab" the same prop (<see cref="BotGrabber.TryGrabSpecific"/>'s
/// TryClaim is the same unconditional stub PhysicsGun.cs's own is - see that
/// method's file comment) - that contention (a tug-of-war, a stolen throw target)
/// is deliberately left visible rather than papered over, since it is exactly what
/// a networked multi-peer grab race will look like once ownership arbitration is
/// real. The one piece of scene-wide, run-once setup this class does - converting
/// <see cref="RagdollDummyName"/> into a ragdoll - guards itself idempotent (see
/// <see cref="SpawnRagdollDummy"/>'s own comment) so N bots attaching to the same
/// scene do not each retry it and spam a failure log once real success has already
/// happened.
/// </summary>
public sealed class BotBrain : EntityScript
{
    private enum State
    {
        /// <summary>Walking to a random point in the arena, scanning for something to grab.</summary>
        Wander,
        /// <summary>Walking straight at a chosen prop, then attempting to grab it.</summary>
        Approach,
        /// <summary>Holding a grabbed prop on the same spring PhysicsGun uses, dragging it
        /// around for a while before flinging or dropping it.</summary>
        Carry,
        /// <summary>Backing away from wherever it just got stuck, then picking a fresh
        /// wander point - the one and only "unstick" manoeuvre; see this class's own
        /// file comment on why this state alone cannot itself wedge.</summary>
        Recover,
    }

    // ── Wander ───────────────────────────────────────────────────────────────────
    /// <summary>Ground speed while wandering or approaching, in m/s.</summary>
    public float WalkSpeed = 3.0f;

    /// <summary>Ground speed while dragging a held prop, in m/s - slower than a plain
    /// walk so the hold spring (see <see cref="BotGrabber.Drive"/>) can keep up instead
    /// of endlessly lagging behind a prop it can never quite catch.</summary>
    public float CarrySpeed = 1.8f;

    /// <summary>Arena bounds random wander points are sampled from (world X/Z). Set from
    /// the scene this bot is placed in - see this project's BotTest.scene.toml for the
    /// test arena's values and the hand-off comment for the main Sandbox arena's.</summary>
    public float ArenaMinX = -8.0f;
    public float ArenaMaxX = 8.0f;
    public float ArenaMinZ = -8.0f;
    public float ArenaMaxZ = 8.0f;

    /// <summary>Close enough to a wander point to call it "arrived" and pick a new one.</summary>
    public float WanderArrivalRadius = 0.75f;

    /// <summary>How far away a grabbable prop can be noticed while wandering, in metres.</summary>
    public float NoticeRadius = 9.0f;

    /// <summary>How often, in seconds, Wander re-scans for a grabbable prop to chase.</summary>
    public float ScanInterval = 0.5f;

    // ── Approach / grab ──────────────────────────────────────────────────────────
    /// <summary>Eye-level height above this entity's feet, for aim origins and the
    /// carry hold point - the same role FirstPersonPlayer.EyeHeight plays.</summary>
    public float EyeHeight = 1.5f;

    /// <summary>Horizontal distance to a target at which the bot stops and tries to
    /// grab it, in metres - PhysicsGun's MaxGrabDistance played in reverse (how close
    /// this bot walks in, rather than how far a human's ray reaches out).</summary>
    public float GrabRange = 3.0f;

    /// <summary>How far in front of eye level the aim ray starts, in metres - guards
    /// against the Character Controller's own inner Jolt body self-hitting the ray,
    /// exactly like PhysicsGun.RayStartOffset.</summary>
    public float RayStartOffset = 0.35f;

    /// <summary>Hard cap on time spent in Approach before giving up on this target,
    /// in seconds - the timer half of the "cannot reach it, give up" contract.</summary>
    public float ApproachTimeout = 10.0f;

    /// <summary>Consecutive failed grab attempts (in range, but the raycast still
    /// missed - blocked, or the target slipped away) before giving up early rather
    /// than waiting out the full <see cref="ApproachTimeout"/>.</summary>
    public int MaxGrabMisses = 6;

    /// <summary>How long a target that was just given up on (unreachable, or just
    /// thrown) is excluded from re-selection, in seconds - stops the bot immediately
    /// re-picking the exact same prop it just failed on or just flung away.</summary>
    public float TargetCooldown = 4.0f;

    // ── Carry / throw ────────────────────────────────────────────────────────────
    /// <summary>How long a prop is held before this bot forces itself to let go, in
    /// seconds - the hold-timeout half of "held something too long, give up".</summary>
    public float CarryDuration = 2.5f;

    /// <summary>How far in front of eye level the held prop is driven, in metres.</summary>
    public float HoldDistance = 1.6f;

    /// <summary>Fraction of grabs that end in a throw rather than a plain drop - most
    /// of the point of this bot is making a mess, but an occasional gentle drop is
    /// closer to how a real player actually plays than throwing every single time.</summary>
    public float ThrowChance = 0.75f;

    // ── Stuck watchdog / recover ─────────────────────────────────────────────────
    /// <summary>How often the stuck watchdog checks for horizontal progress, in
    /// seconds - also how long a stall has to persist before it counts as stuck.</summary>
    public float StuckCheckInterval = 3.0f;

    /// <summary>Minimum horizontal distance the bot must cover every
    /// <see cref="StuckCheckInterval"/> to NOT count as stuck, in metres.</summary>
    public float StuckDistanceThreshold = 0.5f;

    /// <summary>How long Recover backs away before picking a new wander point, in
    /// seconds - a fixed duration, not a "until unstuck" condition; see this class's
    /// own file comment on why that is what keeps Recover itself unwedgeable.</summary>
    public float RecoverBackupDuration = 0.6f;

    /// <summary>Trailing window, in seconds, <see cref="RecentRecoverCount"/> counts
    /// Recover entries within. This only decides how much history to keep - "how
    /// many is too many" is <see cref="BotSoakMonitor"/>'s own threshold to apply,
    /// not a judgement this class makes about itself.</summary>
    public float RecoverLoopWindow = 30.0f;

    // ── Ragdoll (optional) ───────────────────────────────────────────────────────
    /// <summary>Name of a bare-transform "dummy" entity in the scene, if any -
    /// converted into a ragdoll on attach (see <see cref="SpawnRagdollDummy"/>) and
    /// folded into the same grabbable pool as every crate, so this bot occasionally
    /// grabs, carries and flings a ragdoll limb instead of a prop with no separate
    /// code path. Missing from the scene is fine; the bot just never sees one.</summary>
    public string RagdollDummyName = "Ragdoll Dummy";

    /// <summary>Project-relative skeleton asset <see cref="Ragdoll.Spawn"/> reads for
    /// bone names and bind pose - see that method's own doc comment. Only its
    /// skeleton matters; nothing about this asset's rendering is used.</summary>
    public string RagdollSkeletonPath = "project://assets/models/Human/Human.gltf";

    /// <summary>How many times this bot has entered Recover within the trailing
    /// <see cref="RecoverLoopWindow"/> seconds - read by <see cref="BotSoakMonitor"/>
    /// as its "stuck in a loop its own watchdog cannot clear" signal. Meaningless
    /// without a threshold to compare against, which is deliberately not this
    /// class's decision to make about itself.</summary>
    public int RecentRecoverCount => _recoverTimes.Count;

    /// <summary>This bot's own <see cref="BotGrabber.ConsecutiveRefusals"/> -
    /// <see cref="BotSoakMonitor"/>'s starvation signal (a bot repeatedly denied a
    /// claim while others cycle props freely). Always 0 offline - see that
    /// property's own comment on why a real refusal needs an actual host.</summary>
    public int ConsecutiveClaimRefusals => _grabber.ConsecutiveRefusals;

    /// <summary>The prop this bot currently holds, or an invalid entity -
    /// <see cref="BotSoakMonitor"/> reads this across every bot each check tick to
    /// notice a prop's holder changing hands too fast (its thrash signal).</summary>
    public Entity HeldEntity => _grabber.Held;

    private State _state = State.Wander;
    private float _stateTimer;
    private float _clock;
    private float _scanTimer;
    private bool _willThrow;

    private Vector3 _wanderPoint;
    private Vector3 _facing = Vector3.UnitZ;
    private Vector3 _recoverFacing = -Vector3.UnitZ;

    private Entity _target;
    private int _grabMisses;
    private Entity _avoidTarget;
    private float _avoidUntil;

    private Vector3 _stuckAnchor;
    private float _stuckTimer;

    private readonly BotGrabber _grabber = new();
    private readonly Queue<float> _recoverTimes = new();

    public override void OnAttach()
    {
        // Lets BotSoakMonitor enumerate every bot without a registry - see this
        // class's own file comment on multiple bots. Never read by this class
        // itself; Self.GetScript<BotBrain>() from another bot's own code would be
        // the wrong tool anyway, since decisions here are never shared.
        Tags.Add(Self, Tags.Create("bot"));

        // Same idempotent tagging PhysicsGun.OnAttach does for a human player's gun -
        // needed here because a scene with this bot might not also carry a
        // PhysicsGun (this project's own BotTest.scene.toml does not), and even in a
        // scene that does, attach order between the two scripts is not guaranteed.
        // Tags.Add is idempotent, so running this from both scripts when both are
        // present is harmless, same as PropSpawner independently tagging its own
        // spawned props rather than routing through PhysicsGun for it.
        Entity props = Scene.Find("Props");
        if (props.IsValid)
        {
            TagId grabbable = Tags.Create("grabbable");
            for (int i = 0; i < props.ChildCount; i++)
            {
                Tags.Add(props.GetChild(i), grabbable);
            }
        }
        else
        {
            Log.Warn("[Sandbox] BotBrain: scene has no 'Props' entity; only props spawned at runtime will be grabbable.");
        }

        SpawnRagdollDummy();

        _wanderPoint = PickWanderPoint();
        _facing = HorizontalDirection(_wanderPoint - Self.Position);
        _stuckAnchor = Self.Position;
    }

    public override void OnUpdate(float deltaTime)
    {
        _clock += deltaTime;
        _stateTimer += deltaTime;

        // Recover already carries its own bounded exit condition (a timer, not a
        // distance check - see this class's own file comment), so re-running the
        // watchdog while already recovering would only ever re-trigger the same
        // recovery for the same reason.
        if (_state != State.Recover)
        {
            UpdateStuckWatchdog(deltaTime);
        }

        switch (_state)
        {
            case State.Wander:
                TickWander(deltaTime);
                break;
            case State.Approach:
                TickApproach(deltaTime);
                break;
            case State.Carry:
                TickCarry(deltaTime);
                break;
            case State.Recover:
                TickRecover();
                break;
        }
    }

    // ── Wander ───────────────────────────────────────────────────────────────────
    private void TickWander(float deltaTime)
    {
        if (StepToward(_wanderPoint, WalkSpeed))
        {
            _wanderPoint = PickWanderPoint();
        }

        _scanTimer += deltaTime;
        if (_scanTimer < ScanInterval)
        {
            return;
        }
        _scanTimer = 0.0f;

        Entity found = FindNearestGrabbable();
        if (found.IsValid)
        {
            EnterApproach(found);
        }
    }

    /// <summary>Nearest tagged prop within <see cref="NoticeRadius"/>, skipping
    /// whatever is currently on cooldown (see <see cref="TargetCooldown"/>) and
    /// anything already gone.</summary>
    private Entity FindNearestGrabbable()
    {
        Span<Entity> buffer = stackalloc Entity[64];
        TagId grabbable = Tags.Create("grabbable");
        int count = Tags.GetEntitiesWith(grabbable, buffer);

        Entity best = default;
        float bestDistSq = NoticeRadius * NoticeRadius;
        for (int i = 0; i < count; i++)
        {
            Entity candidate = buffer[i];
            if (!candidate.IsValid || candidate == Self || !World.IsValid(candidate))
            {
                continue;
            }
            if (candidate == _avoidTarget && _clock < _avoidUntil)
            {
                continue;
            }

            Vector3 delta = Physics.GetPosition(candidate) - Self.Position;
            delta.Y = 0.0f;
            float distSq = delta.LengthSquared();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                best = candidate;
            }
        }
        return best;
    }

    /// <summary>
    /// One-time scene setup: converts the scene's <see cref="RagdollDummyName"/>
    /// entity (a bare transform, if the scene authors one) into a physics ragdoll
    /// and tags every resulting bone "grabbable" - after this, FindNearestGrabbable/
    /// TryGrabSpecific pick up a limb exactly like any crate, no special-casing,
    /// same as PhysicsGun's own raycast not needing to know a hit is a ragdoll bone.
    /// Missing from the scene, or a failed spawn (bad skeleton path, asset missing
    /// its recognisable humanoid hierarchy - <see cref="Ragdoll.Spawn"/> returns an
    /// empty array either way), just logs and leaves this bot with nothing but
    /// crates to chase - never fatal.
    /// </summary>
    private void SpawnRagdollDummy()
    {
        Entity dummy = Scene.Find(RagdollDummyName);
        if (!dummy.IsValid)
        {
            return;
        }

        // Idempotency for multiple bots (see this class's own file comment): the
        // FIRST bot to reach here converts and tags the dummy; Ragdoll.Spawn is
        // one-shot (RagdollBuilder.cpp refuses an entity that already carries
        // RagdollBoneComponent, returning an empty array), so every later bot
        // would otherwise retry it for nothing and log a spurious failure. The
        // "grabbable" tag - only ever added here after a REAL success below -
        // doubles as that success flag.
        TagId grabbable = Tags.Create("grabbable");
        if (Tags.Has(dummy, grabbable))
        {
            return;
        }

        Entity[] bones = Ragdoll.Spawn(dummy, RagdollSkeletonPath);
        if (bones.Length == 0)
        {
            Log.Warn($"[Sandbox] BotBrain ({Self.Name}): Ragdoll.Spawn failed for '{RagdollDummyName}' (skeleton '{RagdollSkeletonPath}') - no ragdoll target this run.");
            return;
        }

        // bones[0] is the pelvis (dummy itself, same id); every other entry is a
        // fresh bone entity - Ragdoll.Spawn hands back the exact set
        // RagdollComponent tracks internally, so no separate diff/discovery step
        // is needed here any more.
        foreach (Entity bone in bones)
        {
            Tags.Add(bone, grabbable);
        }

        Log.Info($"[Sandbox] BotBrain: '{RagdollDummyName}' is now a {bones.Length}-bone ragdoll and grabbable like any prop.");
    }

    // ── Approach ─────────────────────────────────────────────────────────────────
    private void TickApproach(float deltaTime)
    {
        if (!_target.IsValid || !World.IsValid(_target))
        {
            EnterWander();
            return;
        }

        if (_grabber.HasPendingClaim)
        {
            // A real ownership round trip is in flight (client only - see
            // BotGrabber's own file comment; offline this branch is never taken at
            // all, since a claim always grants synchronously). Stand still and
            // poll; BotGrabber owns its own ClaimTimeoutSeconds, so a refused or
            // lost claim resolves on its own without this needing a second timer.
            CharacterController.Move(Self, Vector3.Zero, 0.0f);
            _grabber.PollPendingClaim(deltaTime, Self);

            if (_grabber.HeldValid)
            {
                EnterCarry();
                return;
            }
            if (!_grabber.HasPendingClaim)
            {
                // Timed out unanswered this frame - an ordinary miss, not a new
                // failure path: the existing grab-miss counter and
                // ApproachTimeout below already funnel a stalled Approach into
                // Recover without a separate escape hatch.
                _grabMisses++;
                if (_grabMisses >= MaxGrabMisses)
                {
                    EnterRecover("a claim on the target timed out unanswered");
                    return;
                }
            }

            if (_stateTimer >= ApproachTimeout)
            {
                EnterRecover("could not reach the target in time");
            }
            return;
        }

        Vector3 targetPos = Physics.GetPosition(_target);
        Vector3 flat = Flatten(targetPos - Self.Position);
        float distance = flat.Length();

        if (distance > GrabRange)
        {
            StepToward(targetPos, WalkSpeed);
        }
        else
        {
            // Close enough: stop advancing and try the grab. A bot does not need to
            // be pixel-perfect aligned the way a human aiming with a mouse does, so
            // this aims the ray straight at the target rather than at wherever
            // _facing happens to point.
            CharacterController.Move(Self, Vector3.Zero, 0.0f);
            if (flat.LengthSquared() > 0.0001f)
            {
                _facing = flat / distance;
            }

            Vector3 origin = Self.Position + Vector3.UnitY * EyeHeight + _facing * RayStartOffset;
            Vector3 aimDir = Vector3.Normalize(targetPos - origin);

            if (_grabber.TryGrabSpecific(_target, Self, origin, aimDir, GrabRange + RayStartOffset + 1.0f))
            {
                EnterCarry();
                return;
            }

            if (!_grabber.HasPendingClaim)
            {
                _grabMisses++;
                if (_grabMisses >= MaxGrabMisses)
                {
                    EnterRecover("could not line up a grab on the target");
                    return;
                }
            }
        }

        if (_stateTimer >= ApproachTimeout)
        {
            EnterRecover("could not reach the target in time");
        }
    }

    // ── Carry ────────────────────────────────────────────────────────────────────
    private void TickCarry(float deltaTime)
    {
        if (!_grabber.HeldValid || !World.IsValid(_grabber.Held))
        {
            // The held prop vanished from under us (destroyed some other way) -
            // nothing to release, just stop pretending we are still carrying it.
            _grabber.Release(Self);
            EnterWander();
            return;
        }

        // Keep wandering while carrying - dragging a held prop through the arena is
        // exactly the "make a mess" exercise this bot exists for, and it doubles as
        // more stuck-watchdog coverage: if the prop itself wedges the bot against
        // geometry, the watchdog and this state's own CarryDuration timer below both
        // still force a release regardless of why.
        StepToward(_wanderPoint, CarrySpeed);

        Vector3 holdTarget = Self.Position + Vector3.UnitY * EyeHeight + _facing * HoldDistance;
        _grabber.Drive(holdTarget, deltaTime);

        if (_stateTimer < CarryDuration)
        {
            return;
        }

        if (_willThrow)
        {
            Vector3 right = Vector3.Cross(_facing, Vector3.UnitY);
            _grabber.Throw(_facing, right, Self);
        }
        else
        {
            _grabber.Release(Self);
        }

        // Cooldown the just-released prop too, so Wander's next scan does not
        // immediately re-pick the thing sitting right in front of the bot.
        _avoidTarget = _target;
        _avoidUntil = _clock + TargetCooldown;
        EnterWander();
    }

    // ── Recover ──────────────────────────────────────────────────────────────────
    private void TickRecover()
    {
        CharacterController.Move(Self, _recoverFacing, WalkSpeed);
        if (_stateTimer >= RecoverBackupDuration)
        {
            EnterWander();
        }
    }

    /// <summary>Horizontal progress check shared by Wander/Approach/Carry. Stalling
    /// for a full <see cref="StuckCheckInterval"/> - blocked by geometry, wedged
    /// against a held prop, anything - forces Recover regardless of which state or
    /// why, which is what makes "has not moved in N seconds" a hard guarantee rather
    /// than something each state has to remember to check on its own.</summary>
    private void UpdateStuckWatchdog(float deltaTime)
    {
        _stuckTimer += deltaTime;
        if (_stuckTimer < StuckCheckInterval)
        {
            return;
        }

        Vector3 delta = Self.Position - _stuckAnchor;
        delta.Y = 0.0f;
        float moved = delta.Length();

        _stuckTimer = 0.0f;
        _stuckAnchor = Self.Position;

        if (moved < StuckDistanceThreshold)
        {
            EnterRecover($"made less than {StuckDistanceThreshold:0.##}m of progress in {StuckCheckInterval:0.##}s");
        }
    }

    // ── State transitions ────────────────────────────────────────────────────────
    private void EnterWander()
    {
        _state = State.Wander;
        _stateTimer = 0.0f;
        _target = default;
        _grabMisses = 0;
        _scanTimer = 0.0f;
        _wanderPoint = PickWanderPoint();
    }

    private void EnterApproach(Entity target)
    {
        _state = State.Approach;
        _stateTimer = 0.0f;
        _target = target;
        _grabMisses = 0;
    }

    private void EnterCarry()
    {
        _state = State.Carry;
        _stateTimer = 0.0f;
        _willThrow = AetherCore.Random.Value < ThrowChance;
    }

    private void EnterRecover(string reason)
    {
        Log.Warn($"[Sandbox] BotBrain ({Self.Name}): recovering ({reason}) - giving up current target and backing off.");
        if (_grabber.HeldValid)
        {
            _grabber.Release(Self);
        }
        if (_target.IsValid)
        {
            _avoidTarget = _target;
            _avoidUntil = _clock + TargetCooldown;
        }
        _target = default;
        _state = State.Recover;
        _stateTimer = 0.0f;
        _recoverFacing = -_facing;

        // Rolling window for RecentRecoverCount (see that property's own comment) -
        // BotSoakMonitor's "stuck in a loop its own watchdog cannot clear" signal.
        _recoverTimes.Enqueue(_clock);
        while (_recoverTimes.Count > 0 && _clock - _recoverTimes.Peek() > RecoverLoopWindow)
        {
            _recoverTimes.Dequeue();
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────────
    /// <summary>Faces and walks straight at a world point, ignoring height - the one
    /// and only steering primitive this bot has; see this class's own file comment on
    /// why there is no more than this. Returns true once within
    /// <see cref="WanderArrivalRadius"/> (and stops the character controller); the
    /// caller decides what "arrived" should mean - pick a new wander point, stop
    /// trying to catch up to something else, and so on.</summary>
    private bool StepToward(Vector3 worldPoint, float speed)
    {
        Vector3 flat = Flatten(worldPoint - Self.Position);
        float distSq = flat.LengthSquared();
        if (distSq <= WanderArrivalRadius * WanderArrivalRadius)
        {
            CharacterController.Move(Self, Vector3.Zero, 0.0f);
            return true;
        }
        _facing = flat / MathF.Sqrt(distSq);
        CharacterController.Move(Self, _facing, speed);
        return false;
    }

    private Vector3 PickWanderPoint()
        => new(AetherCore.Random.Range(ArenaMinX, ArenaMaxX), 0.0f, AetherCore.Random.Range(ArenaMinZ, ArenaMaxZ));

    private static Vector3 Flatten(Vector3 v) => new(v.X, 0.0f, v.Z);

    private static Vector3 HorizontalDirection(Vector3 v)
    {
        Vector3 flat = Flatten(v);
        float lengthSq = flat.LengthSquared();
        return lengthSq > 0.0001f ? flat / MathF.Sqrt(lengthSq) : Vector3.UnitZ;
    }
}
