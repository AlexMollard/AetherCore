using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The actuation half of the AI player's "physics gun": exactly the primitives
/// <see cref="PhysicsGun"/> drives a held prop with (<see cref="Physics.Raycast"/>,
/// <see cref="Physics.SetGravityFactor"/>, <see cref="Physics.SetLinearVelocity"/>,
/// <see cref="Physics.AddImpulseAtPoint"/>) wired to a scripted holder instead of a
/// mouse. This class does not decide WHEN to grab, hold or throw - that is
/// <see cref="BotBrain"/>'s state machine, which calls <see cref="TryGrabSpecific"/>,
/// <see cref="PollPendingClaim"/>, <see cref="Drive"/>, <see cref="Throw"/> and
/// <see cref="Release"/> as pure actuation. Splitting brain from gun this way is what
/// <see cref="BotBrain"/>'s own file comment means by "decisions separable from
/// actuation" - a networked brain would still call these exact same methods, just
/// gated to run only on the authority.
///
/// REAL OWNERSHIP CLAIM, SAME DISCIPLINE AS PhysicsGun.cs: <see cref="TryClaim"/>
/// used to be an unconditional stub; it now calls <see cref="Net.RequestOwnership"/>
/// and <see cref="Net.IsOwner"/> for real, matching PhysicsGun.cs's own TryGrab/
/// PollPendingClaim/BeginHold split exactly (landed there first - see that file's
/// own comments for the fuller reasoning) so a bot and a human physgun answer "what
/// happens while a claim is pending" the same way rather than inventing two
/// disciplines for the same problem. Offline (or already the owner), the grant is
/// synchronous - <see cref="Net.RequestOwnership"/>'s own doc comment says so - so
/// single-player starts the hold the same frame as before this existed, unchanged.
///
/// THRASH FOUND BY BotSoakMonitor, FIXED HERE, NOT WITH A PER-CONNECTION COOLDOWN:
/// a 10-minute, 3-bot soak logged 11 Thrash anomalies - one prop's holder flipping
/// 4+ times in 5s. The obvious fix (a per-connection cooldown in NetOwnership.cpp)
/// was checked against source and ruled out for this specific problem: every
/// local bot in one process shares one Net connection id (see
/// aether_net_request_ownership's own comment - "the caller's OWN connection
/// id"), so a connection-keyed rule cannot tell "bot 2 stealing from bot 1" from
/// "bot 1 regripping its own drop" - both present the identical id.
/// <see cref="s_claims"/> below is the same-process courtesy this problem
/// actually needs, keyed by the claiming SCRIPT's own <c>Self</c> entity rather
/// than a network connection, with two parts: (1) a prop another local grabber
/// is ACTIVELY holding is never claimable, which is the actual root cause -
/// nothing previously stopped two bots from both succeeding a claim on the same
/// still-held prop in the same or next frame, each overwriting the other's hold
/// - and (2) once released, a DIFFERENT claimant still waits out
/// <see cref="ReclaimCooldownSeconds"/> while the releaser itself may reclaim
/// instantly. Deliberately NOT a substitute for a real per-connection rule in
/// NetOwnership.cpp, which remains the right fix for genuine cross-connection
/// thrash once an actual second peer exists to test it against - that is engine
/// work in a file this project does not own, reported rather than attempted here.
///
/// One instance is owned per bot (constructed as a plain field on <see cref="BotBrain"/>,
/// never static) for the same reason PhysicsGun.cs's own file comment gives for
/// <c>_held</c>: a static registry would need cross-peer reconciliation, an instance
/// field is naturally per-authority. <see cref="s_claims"/> is the one deliberate
/// exception, and its own comment explains why: it describes a fact ABOUT THE
/// PROP (who holds it, or last released it, and when), not a player's private
/// decision state, so it does not have that reconciliation problem - every local
/// peer in a real build would compute the same soft courtesy independently, and
/// the real cross-peer authority lives elsewhere.
/// </summary>
public sealed class BotGrabber
{
    /// <summary>Spring "P" term, 1/s - see PhysicsGun.HoldStiffness's own comment.</summary>
    public float HoldStiffness = 45.0f;

    /// <summary>Spring "D" term, 1/s - see PhysicsGun.HoldDamping's own comment.</summary>
    public float HoldDamping = 14.0f;

    /// <summary>Hard clamp on the spring's commanded speed, in m/s.</summary>
    public float MaxHoldSpeed = 22.0f;

    /// <summary>Throw impulse magnitude, in newton-seconds.</summary>
    public float ThrowImpulseStrength = 22.0f;

    /// <summary>How far off-centre the throw impulse lands, in metres, so a thrown
    /// prop tumbles instead of sliding flat - see PhysicsGun.ThrowOffCenter.</summary>
    public float ThrowOffCenter = 0.15f;

    /// <summary>Held-prop tint, matching PhysicsGun's own visual "something is
    /// held" convention but a different hue so a screenshot can tell at a glance
    /// whether the human's gun or the bot's is holding a given prop.</summary>
    public Vector3 HeldEmissiveTint = new(0.85f, 0.35f, 0.1f);

    /// <summary>Seconds a claim may sit unanswered before this bot gives up on it -
    /// see PhysicsGun.ClaimTimeoutSeconds's own comment (a silent host refusal and a
    /// lost request both look identical from here: nothing ever arrives).</summary>
    public float ClaimTimeoutSeconds = 1.5f;

    /// <summary>How long, in seconds, a DIFFERENT local grabber must wait after a
    /// release before it may claim the same prop - see this class's own file
    /// comment on why this exists and why it is keyed by Self, not a connection.
    /// 0.3s: long enough to break the sub-frame re-contest bursts BotSoakMonitor's
    /// Thrash check found (an offline claim grants with no round trip at all, so
    /// back-to-back local grabs can happen within the same or next tick), short
    /// enough to never be perceptible to a second claimant - and it is completely
    /// irrelevant to the releaser's own regrip, which is exempt below.</summary>
    public float ReclaimCooldownSeconds = 0.3f;

    /// <summary>Per-prop claim record - see this class's own file comment for why
    /// this is the one deliberate static in this class. <c>Active</c> means some
    /// local grabber currently holds it (a claim by anyone else is refused
    /// outright, regardless of cooldown - this is what actually stops two bots
    /// simultaneously believing they each hold the same prop, the root cause the
    /// cooldown alone did not cover); once released, <c>HolderId</c>/<c>Since</c>
    /// become "who released it, and when" for <see cref="ReclaimCooldownSeconds"/>.
    /// Entries are never pruned: the handful of grabbable props/bones in a scene
    /// make that unnecessary.</summary>
    private readonly record struct ClaimRecord(uint HolderId, float Since, bool Active);

    private static readonly Dictionary<uint, ClaimRecord> s_claims = new();

    /// <summary>The prop currently held, or an invalid entity.</summary>
    public Entity Held { get; private set; }

    public bool HeldValid => Held.IsValid;

    /// <summary>A claim was sent for this prop but not yet answered (client only).
    /// While this is set, the prop is untouched - not tinted, not gravity-zeroed,
    /// not driven - because it is still the replication system's to move until the
    /// grant lands; see this class's own file comment.</summary>
    public bool HasPendingClaim => _pendingClaim.IsValid;

    /// <summary>Consecutive claim attempts refused (timed out unanswered) with no
    /// successful grab in between - reset to 0 by every <see cref="BeginHold"/>.
    /// <see cref="BotSoakMonitor"/>'s starvation signal reads this; a genuine
    /// refusal (as opposed to a plain raycast miss) can only happen with an actual
    /// host connected to say no, so this is architecturally always 0 offline.</summary>
    public int ConsecutiveRefusals { get; private set; }

    private Entity _pendingClaim;
    private float _pendingClaimElapsed;

    /// <summary>
    /// Raycasts from <paramref name="origin"/> along <paramref name="direction"/> and
    /// claims the hit only when it is exactly <paramref name="target"/> - the bot
    /// already committed to that specific prop back when it entered Approach (see
    /// BotBrain's own file comment), so a stray different grabbable prop drifting
    /// through the shot should not be picked up instead. Returns false on any miss
    /// (wrong hit, no line of sight, already held/pending) and leaves everything
    /// untouched; also returns false - not a miss, see <see cref="HasPendingClaim"/> -
    /// when a real claim was just sent and is awaiting a grant. BotBrain distinguishes
    /// the two by checking <see cref="HasPendingClaim"/> after a false return, so a
    /// pending claim is never miscounted as a failed grab attempt.
    /// </summary>
    public bool TryGrabSpecific(Entity target, Entity self, Vector3 origin, Vector3 direction, float maxDistance)
    {
        if (HeldValid || HasPendingClaim)
        {
            return false;
        }

        RaycastHit hit = Physics.Raycast(origin, direction, maxDistance);
        if (!hit.DidHit || !hit.Entity.IsValid || hit.Entity == self || hit.Entity != target)
        {
            return false;
        }
        if (!Tags.Has(hit.Entity, Tags.Create("grabbable")))
        {
            return false;
        }
        if (!CanClaim(hit.Entity, self))
        {
            return false;
        }
        if (!TryClaim(hit.Entity))
        {
            return false;
        }

        if (Net.IsOwner(hit.Entity))
        {
            // Offline, or already ours: the change is immediate in both cases, so
            // the hold starts this same frame exactly like it did before ownership
            // existed at all - single-player is unaffected.
            BeginHold(hit.Entity, self);
            return true;
        }

        // Sent to the host; the answer is not in yet. BotBrain's TickApproach polls
        // via PollPendingClaim every frame after this until it resolves.
        _pendingClaim = hit.Entity;
        _pendingClaimElapsed = 0.0f;
        return false;
    }

    /// <summary>Polls a sent-but-unanswered claim once per frame while nothing is
    /// held. Starts the hold the instant <see cref="Net.IsOwner"/> flips true - from
    /// the prop's CURRENT position (<see cref="BeginHold"/> reads it fresh), not the
    /// original raycast hit point, since a delayed grant may land several frames
    /// after this bot's aim point moved - and gives up cleanly after
    /// <see cref="ClaimTimeoutSeconds"/> if the host silently refuses or the
    /// candidate is destroyed while waiting.</summary>
    public void PollPendingClaim(float deltaTime, Entity self)
    {
        if (!_pendingClaim.IsValid)
        {
            return;
        }
        if (Net.IsOwner(_pendingClaim))
        {
            BeginHold(_pendingClaim, self);
            return;
        }
        _pendingClaimElapsed += deltaTime;
        if (_pendingClaimElapsed >= ClaimTimeoutSeconds)
        {
            _pendingClaim = default;
            _pendingClaimElapsed = 0.0f;
            ConsecutiveRefusals++;
        }
    }

    /// <summary>Starts actually driving <paramref name="prop"/> once ownership is
    /// confirmed ours - shared by the immediate-grant path in
    /// <see cref="TryGrabSpecific"/> and the delayed-grant path in
    /// <see cref="PollPendingClaim"/>. Marks the claim Active in
    /// <see cref="s_claims"/> so no other local grabber can claim the same prop
    /// while this one actually holds it - see this class's own file comment on
    /// why that check, not just the release cooldown, is what stops two bots
    /// simultaneously believing they each hold the same prop.</summary>
    private void BeginHold(Entity prop, Entity self)
    {
        Held = prop;
        ConsecutiveRefusals = 0;
        _pendingClaim = default;
        _pendingClaimElapsed = 0.0f;
        s_claims[prop.Id] = new ClaimRecord(self.Id, Time.TotalTime, true);

        // Wake a sleeping prop the instant it is grabbed, then make it weightless
        // for the hold - identical sequence to PhysicsGun.BeginHold, same reasons.
        Physics.SetBodyActive(Held, true);
        Physics.SetGravityFactor(Held, 0.0f);
        Held.Material.SetEmissive(HeldEmissiveTint);
    }

    /// <summary>
    /// The ownership boundary: sends the claim request and nothing more - it does
    /// NOT mean the prop is ours (the host may still silently refuse a request for
    /// something another connected player already holds). TryGrabSpecific checks
    /// Net.IsOwner right after this returns to tell an immediate grant (offline, or
    /// already ours) from one still in flight; PollPendingClaim is what actually
    /// notices a delayed grant or gives up on a refused/lost one. Returns false only
    /// when <paramref name="prop"/> is not a live handle.
    /// </summary>
    private static bool TryClaim(Entity prop) => Net.RequestOwnership(prop);

    /// <summary>True when <paramref name="claimant"/> may claim <paramref name="prop"/>
    /// right now: nobody has ever claimed it, <paramref name="claimant"/> is the one
    /// who already holds or most recently released it (always allowed - a regrip is
    /// not thrash), or it is free AND has been for at least
    /// <see cref="ReclaimCooldownSeconds"/>. A prop another local grabber is
    /// ACTIVELY holding is never claimable regardless of timing - see this class's
    /// own file comment for why that check exists at all.</summary>
    private bool CanClaim(Entity prop, Entity claimant)
    {
        if (!s_claims.TryGetValue(prop.Id, out ClaimRecord record))
        {
            return true;
        }
        if (record.HolderId == claimant.Id)
        {
            return true;
        }
        if (record.Active)
        {
            return false;
        }
        return Time.TotalTime - record.Since >= ReclaimCooldownSeconds;
    }

    /// <summary>The same critically-damped velocity spring as PhysicsGun.DriveHeldProp -
    /// see that method's own comment for why this drives velocity, never a position
    /// write, and so keeps colliding with the world on the way to its target.</summary>
    public void Drive(Vector3 targetPoint, float deltaTime)
    {
        if (!HeldValid)
        {
            return;
        }

        Vector3 currentPosition = Physics.GetPosition(Held);
        Vector3 error = targetPoint - currentPosition;

        Vector3 currentVelocity = Physics.GetLinearVelocity(Held);
        Vector3 desiredVelocity = error * HoldStiffness;
        Vector3 newVelocity = Vector3.Lerp(currentVelocity, desiredVelocity, Math.Clamp(HoldDamping * deltaTime, 0.0f, 1.0f));

        float speed = newVelocity.Length();
        if (speed > MaxHoldSpeed)
        {
            newVelocity = newVelocity / speed * MaxHoldSpeed;
        }

        Physics.SetLinearVelocity(Held, newVelocity);
    }

    /// <summary>Lets go without throwing - restores gravity, clears spin and tint,
    /// and hands ownership back to the host so someone else (or this bot, later)
    /// can claim it. Also abandons any still-pending claim, since a bot deciding to
    /// give up (Recover) should not keep polling for a hold it no longer wants.
    /// Marks the claim inactive in <see cref="s_claims"/> (see
    /// <see cref="CanClaim"/>) so this exact grabber may reclaim the prop
    /// instantly while any other local grabber waits out the cooldown.</summary>
    public void Release(Entity self)
    {
        if (HeldValid)
        {
            Physics.SetGravityFactor(Held, 1.0f);
            Physics.SetAngularVelocity(Held, Vector3.Zero);
            Held.Material.SetEmissive(Vector3.Zero);
            Net.ReleaseOwnership(Held);
            s_claims[Held.Id] = new ClaimRecord(self.Id, Time.TotalTime, false);
        }
        Held = default;
        _pendingClaim = default;
        _pendingClaimElapsed = 0.0f;
    }

    /// <summary>Flings the held prop along <paramref name="forward"/>, offset by
    /// <paramref name="rightAxis"/> so it tumbles - PhysicsGun.Throw's exact recipe -
    /// then releases it (and its ownership, recording <paramref name="self"/> as
    /// the releaser - see <see cref="Release"/>).</summary>
    public void Throw(Vector3 forward, Vector3 rightAxis, Entity self)
    {
        if (!HeldValid)
        {
            return;
        }
        Vector3 offCentrePoint = Physics.GetPosition(Held) + rightAxis * ThrowOffCenter;
        Physics.AddImpulseAtPoint(Held, forward * ThrowImpulseStrength, offCentrePoint);
        Release(self);
    }
}
