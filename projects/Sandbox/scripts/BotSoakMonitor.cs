using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Turns the bot fleet into an unattended soak test. Runs for <see cref="Duration"/>
/// seconds (0 = run forever, stream anomalies, never conclude - useful for
/// interactive poking), watching every "grabbable"-tagged prop/ragdoll-bone and
/// every "bot"-tagged <see cref="BotBrain"/> for the failure modes this engine can
/// actually produce, then emits one machine-readable verdict line any external
/// tool - a human via <c>aether-ctl console.logs</c>, or a future CI run - can
/// scrape without watching the viewport. "Something broke" with no further detail
/// is exactly what this format refuses to be: every anomaly line names the entity,
/// its id, the simulation time, and the specific numbers that triggered it.
///
/// LOG FORMAT (both greppable via console.logs {"contains": "SOAK_ANOMALY"} /
/// {"contains": "SOAK_VERDICT"}):
///   [Sandbox] SOAK_ANOMALY: type=&lt;Kind&gt; entity='&lt;name&gt;' id=&lt;id&gt; time=&lt;sec&gt; &lt;details...&gt;
///   [Sandbox] SOAK_VERDICT: PASS|FAIL duration=&lt;sec&gt;s anomalies=&lt;count&gt;
///
/// READ-ONLY, DECOUPLED FROM BotBrain: this script never steers, grabs, or throws
/// anything - only Physics/CharacterController/World queries. Exactly one of this
/// script exists per scene, watching however many <see cref="BotBrain"/> instances
/// are present (see that class's own file comment on multiple bots needing no
/// coordination) - it is not one monitor per bot.
///
/// ANOMALY KINDS:
///  - NonFinitePosition / NonFiniteVelocity: NaN/Infinity, either kind is always a bug.
///  - OutOfBounds: X/Z beyond the arena bounds by more than
///    <see cref="OutOfBoundsMargin"/> - thrown clean out of the (fully walled)
///    arena, checked before FellThroughFloor since escaping horizontally can only
///    happen through a wall, which is the more specific finding.
///  - FellThroughFloor: still within the arena's horizontal footprint but Y below
///    <see cref="FloorFallY"/> - sank through the ground slab in place.
///  - InsaneSpeed: linear speed above <see cref="InsaneSpeed"/> - the constraint
///    solver injecting energy (a "joint explosion") reads as a huge, sustained
///    velocity, well beyond anything a single throw or grab-spring clamp
///    (BotGrabber.MaxHoldSpeed) could ever produce on its own.
///  - StuckLoop: a bot's own <see cref="BotBrain.RecentRecoverCount"/> has crossed
///    <see cref="RecoverLoopThreshold"/> - its watchdog IS firing, repeatedly, and
///    nothing is clearing whatever keeps stalling it.
///  - EntityCountGrowth: the live entity count has grown past its post-warmup
///    baseline by more than <see cref="EntityCountGrowthMargin"/> - something is
///    creating entities without bound (this bot fleet never spawns anything after
///    setup, so growth here always means a leak, not legitimate scene content).
///  - Starvation: a bot's own <see cref="BotBrain.ConsecutiveClaimRefusals"/> has
///    crossed <see cref="StarvationThreshold"/> - repeatedly denied a claim while
///    (presumably) others keep cycling props freely. Architecturally always 0 in
///    single-player - a refusal needs an actual host to say no - so this only
///    fires in a genuine multi-peer session.
///  - Thrash: one prop's holder, among the tracked bots, has changed hands
///    <see cref="ThrashThreshold"/>+ times within <see cref="ThrashWindowSeconds"/> -
///    ownership ping-ponging faster than the normal grab-carry-throw cadence
///    could produce. Unlike Starvation this IS observable in single-player, since
///    it only needs several bots to each briefly win the same contested prop in
///    quick succession, not a real network refusal.
///
/// Every anomaly is reported ONCE per entity per kind (a wedged NaN prop does not
/// need re-logging every CheckInterval for the rest of the run) via the
/// per-kind "already reported" sets below.
/// </summary>
public sealed class BotSoakMonitor : EntityScript
{
    // ── Run length ───────────────────────────────────────────────────────────────
    /// <summary>Seconds to run before emitting a verdict and going quiet. 0 means
    /// never conclude - anomalies still stream, just no final PASS/FAIL line.</summary>
    public float Duration = 120.0f;

    /// <summary>How often, in seconds, every tracked entity is re-inspected.</summary>
    public float CheckInterval = 1.0f;

    /// <summary>Seconds to wait before baselining the entity count for
    /// <see cref="EntityCountGrowthMargin"/> - long enough for one-shot setup
    /// (BotBrain.SpawnRagdollDummy's ~10 new bone entities) to finish, so that is
    /// never mistaken for unbounded growth.</summary>
    public float WarmupSeconds = 5.0f;

    // ── Bounds / floor (match the arena this monitor is placed in) ───────────────
    public float ArenaMinX = -10.0f;
    public float ArenaMaxX = 10.0f;
    public float ArenaMinZ = -10.0f;
    public float ArenaMaxZ = 10.0f;

    /// <summary>How far past the arena bounds, in metres, counts as thrown out
    /// rather than just resting near a wall after a hard bounce.</summary>
    public float OutOfBoundsMargin = 3.0f;

    /// <summary>World Y below which something has tunnelled through the ground
    /// slab rather than merely landing somewhere unexpected.</summary>
    public float FloorFallY = -3.0f;

    // ── Physics sanity ───────────────────────────────────────────────────────────
    /// <summary>Linear speed, in m/s, above which something is gaining energy it
    /// has no legitimate source for. Comfortably above BotGrabber.MaxHoldSpeed
    /// (22) and any single throw impulse this bot fleet can impart, so a normal
    /// grab/throw/tug-of-war never crosses it - only a genuine solver blow-up does.</summary>
    public float InsaneSpeed = 60.0f;

    // ── Stuck-loop ───────────────────────────────────────────────────────────────
    /// <summary>Recoveries within a bot's own RecoverLoopWindow that counts as
    /// "stuck in a loop its watchdog cannot clear" rather than ordinary bad luck.</summary>
    public int RecoverLoopThreshold = 4;

    // ── Ownership contention ─────────────────────────────────────────────────────
    /// <summary>Consecutive claim refusals (BotGrabber.ConsecutiveRefusals) that
    /// count as one bot being starved rather than ordinary bad luck. Architecturally
    /// always 0 in single-player - a real refusal needs an actual host to say no -
    /// so this only ever fires in a genuine multi-peer session.</summary>
    public int StarvationThreshold = 5;

    /// <summary>Rolling window, in seconds, holder changes on one prop are counted
    /// within for the thrash check below.</summary>
    public float ThrashWindowSeconds = 5.0f;

    /// <summary>Distinct holder changes on one prop within ThrashWindowSeconds that
    /// count as ping-ponging rather than the normal contention cadence - a healthy
    /// grab-carry-throw cycle hands a prop off roughly every CarryDuration seconds,
    /// well slower than this threshold requires.</summary>
    public int ThrashThreshold = 4;

    // ── Entity-count growth ──────────────────────────────────────────────────────
    /// <summary>How far past the post-warmup baseline the live entity count may
    /// grow before it counts as unbounded rather than noise/rounding.</summary>
    public int EntityCountGrowthMargin = 5;

    private float _clock;
    private float _checkTimer;
    private bool _warmedUp;
    private int _baselineEntityCount;
    private bool _concluded;
    private int _anomalyCount;
    private float _maxSpeedObserved;
    private float _maxDistanceFromOrigin;

    /// <summary>Total ownership handoffs observed on ANY tracked bot across the
    /// whole run - not just the ones that crossed ThrashThreshold. Reported
    /// alongside the verdict so a low anomaly count can be told apart from
    /// grabbing having simply stopped: a cooldown fix that suppresses contention
    /// rather than resolving it would show zero Thrash AND a collapsed handoff
    /// total, whereas a real fix keeps this number healthy while Thrash drops.</summary>
    private int _totalHandoffs;

    private readonly HashSet<uint> _reportedNonFinitePos = new();
    private readonly HashSet<uint> _reportedNonFiniteVel = new();
    private readonly HashSet<uint> _reportedFellThrough = new();
    private readonly HashSet<uint> _reportedOutOfBounds = new();
    private readonly HashSet<uint> _reportedInsaneSpeed = new();
    private readonly HashSet<uint> _reportedStuckLoop = new();
    private readonly HashSet<uint> _reportedStarvation = new();
    private readonly HashSet<uint> _reportedThrash = new();
    private bool _reportedEntityGrowth;

    // propId -> bot id currently believed to hold it, and the rolling timestamps
    // of each time that changed - the thrash check's own bookkeeping.
    private readonly Dictionary<uint, uint> _lastHolderByProp = new();
    private readonly Dictionary<uint, Queue<float>> _holderChangeTimes = new();

    public override void OnUpdate(float deltaTime)
    {
        if (_concluded)
        {
            return;
        }

        _clock += deltaTime;
        _checkTimer += deltaTime;
        if (_checkTimer < CheckInterval)
        {
            return;
        }
        _checkTimer = 0.0f;

        CheckTrackedProps();
        CheckBots();
        CheckEntityGrowth();

        if (Duration > 0.0f && _clock >= Duration)
        {
            Conclude();
        }
    }

    /// <summary>Every "grabbable" entity (crates, ragdoll bones, anything spawned
    /// later) is a real rigid body, so position/velocity come straight from
    /// Physics - the same source BotGrabber itself drives.</summary>
    private void CheckTrackedProps()
    {
        Span<Entity> buffer = stackalloc Entity[256];
        TagId grabbable = Tags.Create("grabbable");
        int count = Tags.GetEntitiesWith(grabbable, buffer);
        for (int i = 0; i < count; i++)
        {
            Entity e = buffer[i];
            if (!e.IsValid || !World.IsValid(e))
            {
                continue;
            }
            Inspect(e, Physics.GetPosition(e), Physics.GetLinearVelocity(e));
        }
    }

    /// <summary>Bots are Character Controllers, not rigid bodies - Physics.GetPosition
    /// queries the wrong subsystem for one (it always reads the rigid-body table),
    /// so a bot's own sanity check reads Entity.Position (backed by its Transform,
    /// always live) and CharacterController.GetVelocity instead.</summary>
    private void CheckBots()
    {
        Span<Entity> buffer = stackalloc Entity[64];
        TagId botTag = Tags.Create("bot");
        int count = Tags.GetEntitiesWith(botTag, buffer);
        for (int i = 0; i < count; i++)
        {
            Entity e = buffer[i];
            if (!e.IsValid || !World.IsValid(e))
            {
                continue;
            }
            Inspect(e, e.Position, CharacterController.GetVelocity(e));

            BotBrain? brain = e.GetScript<BotBrain>();
            if (brain == null)
            {
                continue;
            }

            if (brain.RecentRecoverCount >= RecoverLoopThreshold && _reportedStuckLoop.Add(e.Id))
            {
                Report(e, "StuckLoop", $"recoverCount={brain.RecentRecoverCount} windowSec={brain.RecoverLoopWindow:0}");
            }

            if (brain.ConsecutiveClaimRefusals >= StarvationThreshold && _reportedStarvation.Add(e.Id))
            {
                Report(e, "Starvation", $"consecutiveRefusals={brain.ConsecutiveClaimRefusals}");
            }

            if (brain.HeldEntity.IsValid)
            {
                TrackHolderChange(brain.HeldEntity, e.Id);
            }
        }
    }

    /// <summary>Notices a prop's holder (among the tracked bots) changing hands and
    /// counts how many times that has happened within <see cref="ThrashWindowSeconds"/> -
    /// the thrash signal. A prop still held by the same bot as last check is not a
    /// change; going from "held by A" to "held by B" (with or without a moment
    /// unheld in between, at this once-a-second sampling rate) is.</summary>
    private void TrackHolderChange(Entity prop, uint botId)
    {
        if (_lastHolderByProp.TryGetValue(prop.Id, out uint lastHolder) && lastHolder == botId)
        {
            return;
        }
        _lastHolderByProp[prop.Id] = botId;
        _totalHandoffs++;

        if (!_holderChangeTimes.TryGetValue(prop.Id, out Queue<float>? times))
        {
            times = new Queue<float>();
            _holderChangeTimes[prop.Id] = times;
        }
        times.Enqueue(_clock);
        while (times.Count > 0 && _clock - times.Peek() > ThrashWindowSeconds)
        {
            times.Dequeue();
        }

        if (times.Count >= ThrashThreshold && _reportedThrash.Add(prop.Id))
        {
            Report(prop, "Thrash", $"holderChanges={times.Count} windowSec={ThrashWindowSeconds:0}");
        }
    }

    private void Inspect(Entity e, Vector3 pos, Vector3 vel)
    {
        if (!IsFinite(pos))
        {
            if (_reportedNonFinitePos.Add(e.Id))
            {
                Report(e, "NonFinitePosition", $"position=({pos.X},{pos.Y},{pos.Z})");
            }
        }
        else if (pos.X < ArenaMinX - OutOfBoundsMargin || pos.X > ArenaMaxX + OutOfBoundsMargin
                 || pos.Z < ArenaMinZ - OutOfBoundsMargin || pos.Z > ArenaMaxZ + OutOfBoundsMargin)
        {
            // Horizontally escaped the arena - checked before FellThroughFloor
            // below, since in a fully-walled arena the only way out is through a
            // wall, and that is the more specific finding than "also below the
            // floor plane" once already past the walls' footprint.
            if (_reportedOutOfBounds.Add(e.Id))
            {
                Report(e, "OutOfBounds", $"position=({pos.X:0.00},{pos.Y:0.00},{pos.Z:0.00})");
            }
        }
        else if (pos.Y < FloorFallY)
        {
            if (_reportedFellThrough.Add(e.Id))
            {
                Report(e, "FellThroughFloor", $"position=({pos.X:0.00},{pos.Y:0.00},{pos.Z:0.00})");
            }
        }

        if (IsFinite(pos) && pos.Length() > _maxDistanceFromOrigin)
        {
            _maxDistanceFromOrigin = pos.Length();
        }

        if (!IsFinite(vel))
        {
            if (_reportedNonFiniteVel.Add(e.Id))
            {
                Report(e, "NonFiniteVelocity", $"velocity=({vel.X},{vel.Y},{vel.Z})");
            }
        }
        else if (vel.Length() > InsaneSpeed && _reportedInsaneSpeed.Add(e.Id))
        {
            Report(e, "InsaneSpeed", $"speed={vel.Length():0.0}m/s");
        }

        if (IsFinite(vel) && vel.Length() > _maxSpeedObserved)
        {
            _maxSpeedObserved = vel.Length();
        }
    }

    private void CheckEntityGrowth()
    {
        Span<Entity> buffer = stackalloc Entity[512];
        int count = World.GetEntitiesWithTransform(buffer);

        if (!_warmedUp)
        {
            if (_clock < WarmupSeconds)
            {
                return;
            }
            _warmedUp = true;
            _baselineEntityCount = count;
            return;
        }

        if (!_reportedEntityGrowth && count > _baselineEntityCount + EntityCountGrowthMargin)
        {
            _reportedEntityGrowth = true;
            _anomalyCount++;
            Log.Warn($"[Sandbox] SOAK_ANOMALY: type=EntityCountGrowth time={_clock:0.0} baseline={_baselineEntityCount} current={count}");
        }
    }

    private void Report(Entity e, string kind, string detail)
    {
        _anomalyCount++;
        Log.Warn($"[Sandbox] SOAK_ANOMALY: type={kind} entity='{e.Name}' id={e.Id} time={_clock:0.0} {detail}");
    }

    /// <summary>Verdict line carries trend numbers alongside PASS/FAIL on purpose -
    /// a clean pass with the entity count creeping up, or a max speed quietly
    /// climbing run over run, is a real problem the binary verdict alone would
    /// hide. entitiesStart/End let a leak show up even on a run with zero
    /// individually-flagged EntityCountGrowth anomalies (margin-tolerant by
    /// design); maxSpeed/maxDistFromOrigin are the same idea for slow drift that
    /// never crosses this run's InsaneSpeed/OutOfBounds thresholds outright.
    /// totalHandoffs exists so a low (or zero) Thrash count can be told apart
    /// from grabbing having simply stopped - a fix that suppresses contention by
    /// making claims fail more, rather than resolving the double-claim bug,
    /// would show zero Thrash AND a collapsed handoff total.</summary>
    private void Conclude()
    {
        _concluded = true;
        string verdict = _anomalyCount == 0 ? "PASS" : "FAIL";

        Span<Entity> buffer = stackalloc Entity[512];
        int finalEntityCount = World.GetEntitiesWithTransform(buffer);
        int startEntityCount = _warmedUp ? _baselineEntityCount : finalEntityCount;

        Log.Info($"[Sandbox] SOAK_VERDICT: {verdict} duration={_clock:0.0}s anomalies={_anomalyCount} "
                + $"entitiesStart={startEntityCount} entitiesEnd={finalEntityCount} "
                + $"maxSpeed={_maxSpeedObserved:0.0}m/s maxDistFromOrigin={_maxDistanceFromOrigin:0.0}m "
                + $"totalHandoffs={_totalHandoffs}");
    }

    private static bool IsFinite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z);
}
