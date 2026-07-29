using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Whisper's gun, health, score and death/respawn cycle - everything about a player
/// that shooting touches. One weapon, aimed with the mouse, firing a travelling
/// projectile that the shooter owns and simulates.
/// </summary>
/// <remarks>
/// <para>
/// <b>Every fact about a player is authored by the peer that owns that player.</b>
/// That is the one rule the whole framework runs on and this script introduces no
/// second one. Health is decremented by the VICTIM on the victim's own machine and
/// replicated outward; a kill is added by the KILLER on the killer's own machine;
/// a death is added by the victim. Nothing here writes a replicated field on an
/// entity this peer does not own - the framework refuses that anyway (see
/// <see cref="Net.SetPlayerName"/>), and a value written from the wrong side is
/// overwritten by the owner's next send, which is exactly how "some players' names
/// never showed" happened.
/// </para>
/// <para>
/// <b>Why every call goes through the host.</b> Only the host may originate a
/// <see cref="NetRpcTarget.Client"/> call, and the host accepts a
/// <see cref="NetRpcTarget.Server"/> call only when the SENDER OWNS the entity it is
/// addressed to. So a peer cannot speak to another peer directly, and every exchange
/// below is the same two hops: owner -> host on something the owner owns, then host
/// -> owner of the far entity. Damage and kill credit both take that shape:
/// </para>
/// <code>
///   shooter's Projectile --ReportHit--&gt; host --ApplyHit--&gt;  victim's owner
///   victim  --ReportKill--&gt; host --AwardKill--&gt; killer's owner
/// </code>
/// <para>
/// <b>Health is replicated the way facing and AnimState are</b> - a
/// <c>[Replicated]</c> script field written by the owner, applied by everybody. It is
/// deliberately not a third mechanism: the dead/alive presentation below and the
/// "cannot be shot again" test in <see cref="Projectile"/> both read the same one
/// number on whatever peer is asking.
/// </para>
/// </remarks>
public sealed class PlayerCombat : EntityScript
{
    // ── Weapon and body tuning ──────────────────────────────────────────────────

    /// <summary>Prefab the gun fires. One weapon, so one name.</summary>
    public string ProjectilePrefab = "bullet";

    /// <summary>Health a player spawns and respawns with.</summary>
    public int MaxHealth = 100;

    /// <summary>Health one projectile removes - four shots to a kill.</summary>
    /// <remarks>
    /// Applied by the VICTIM from its own copy of this field, and deliberately NOT
    /// carried in the hit message. The weapon is the same for everybody, so putting
    /// the number on the wire would add nothing except a number a modified shooter
    /// could choose to be 999.
    /// </remarks>
    public int Damage = 25;

    /// <summary>Seconds between shots. Holding the button fires at this rate.</summary>
    public float FireInterval = 0.28f;

    /// <summary>Seconds a dead player waits before coming back at its spawn point.</summary>
    public float RespawnSeconds = 3.0f;

    /// <summary>How far in front of the character the projectile is created.</summary>
    /// <remarks>
    /// Half of "do not shoot yourself": the player's box collider is 1.0 x 1.3, so
    /// 0.9 along the aim direction is outside it from every angle. The other half is
    /// unconditional - <see cref="Projectile"/> never reports a hit on the player its
    /// own shooter owns, whatever the geometry says.
    /// </remarks>
    public float MuzzleOffset = 0.9f;

    /// <summary>How far from the host's copy of the shooter a claimed muzzle may be
    /// before the host refuses the shot.</summary>
    /// <remarks>
    /// The host's copy of a client is about one round trip old, so this cannot be
    /// tight - it is a bound on "spawned a bullet across the map", not a physics
    /// check. Cheap, and the alternative is accepting an arbitrary world position
    /// from a peer.
    /// </remarks>
    public float MaxMuzzleDistance = 4.0f;

    /// <summary>How much faster than <see cref="FireInterval"/> the host will still
    /// accept a shot, as a fraction, to absorb frame quantisation and jitter.</summary>
    public float HostRateTolerance = 0.7f;

    // ── Replicated ──────────────────────────────────────────────────────────────

    /// <summary>This player's health. Written only by its owner - see the class
    /// remarks - and read by every peer for the alive/dead presentation and for the
    /// "already dead, do not hit again" test.</summary>
    [Replicated] public int Health = 100;

    /// <summary>Kills this player has scored, written by this player's own owner when
    /// the host tells it that it landed a killing blow.</summary>
    [Replicated] public int Kills;

    /// <summary>Times this player has died, written by this player's own owner as it
    /// applies the killing blow to itself.</summary>
    [Replicated] public int Deaths;

    /// <summary>Whether this player is up. Derived from <see cref="Health"/> rather
    /// than replicated separately, so there is no way for the two to disagree.</summary>
    public bool IsAlive => Health > 0;

    /// <summary>Seconds until this player comes back, or 0. Owner-local: it is a
    /// countdown on the machine doing the waiting, and no other peer acts on it.</summary>
    public float RespawnIn => _respawnIn;

    // ── Owner-local state ───────────────────────────────────────────────────────

    /// <summary>Directions of shots this peer has asked the host for and has not yet
    /// been handed a projectile for.</summary>
    /// <remarks>
    /// <para>
    /// <b>Why this exists at all.</b> Only the host may allocate a net id, so a
    /// client's projectile is created by the host on request - and the Spawn message
    /// carries a prefab and a position and nothing else. The direction therefore has
    /// to be recovered on the owner when its copy arrives, and this is where the
    /// shot it belongs to is kept.
    /// </para>
    /// <para>
    /// <b>Why matching in order is exact.</b> The request and the resulting Spawn both
    /// travel on the reliable ORDERED channel, so this connection's requests reach the
    /// host in the order it made them and the spawns come back in the order the host
    /// made them. First in, first claimed.
    /// </para>
    /// <para>
    /// Entries still expire (<see cref="PendingShotSeconds"/>). A host that REFUSES a
    /// shot - which in the normal path it never does, because this peer applies the
    /// same rate limit before asking - would otherwise leave an entry that pairs every
    /// later projectile with the wrong shot for the rest of the session.
    /// </para>
    /// </remarks>
    private readonly Queue<PendingShot> _pending = new();

    /// <summary>How long an unclaimed pending shot survives.</summary>
    public float PendingShotSeconds = 1.0f;

    private readonly struct PendingShot(Vector2 direction, float age)
    {
        public readonly Vector2 Direction = direction;
        public readonly float Age = age;

        public PendingShot Older(float deltaTime) => new(Direction, Age + deltaTime);
    }

    private float _sinceFire;
    private float _respawnIn;

    // True while a held fire button belongs to the interface rather than to the gun.
    // See TickOwner: the engine spends a KEY that a UI element acted on, and there is
    // no counterpart for a mouse button, so this is the game's half of the same rule.
    private bool _triggerSpent;

    // Host-side rate gate, kept on the host's copy of the shooter. Negative infinity
    // rather than 0 so the very first shot of a session is not refused for arriving
    // less than one interval after the clock's origin.
    private float _lastAcceptedFire = float.NegativeInfinity;

    // Presentation latches. Same shape as NetPlayerSync's: the per-frame path is a
    // compare, and the first apply happens even when the value is already the default.
    private int _shownHealth = int.MinValue;
    private bool _shownAlive;
    private bool _aliveApplied;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        // Ready to fire immediately rather than one interval after spawning.
        _sinceFire = FireInterval;
        if (Net.HasAuthority(Self))
        {
            Health = MaxHealth;
        }
        Present();
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        // Same shape as NetPlayerSync: the owner DECIDES, everybody APPLIES. On every
        // other peer these fields arrive from the wire and are simply presented.
        // HasAuthority is true offline, so single-player is unaffected.
        if (Net.HasAuthority(Self) && !Time.IsPaused)
        {
            TickOwner(deltaTime);
        }
        Present();
    }

    // ── Owner ───────────────────────────────────────────────────────────────────

    private void TickOwner(float deltaTime)
    {
        _sinceFire += deltaTime;
        AgePendingShots(deltaTime);

        // The UI owns the keyboard AND the mouse while anything in it is focused - a
        // chat message being typed, the pause menu being read. Firing at the Disconnect
        // button is not aiming.
        //
        // The latch is the other half of that, and it is here because the framework has
        // no half to offer. A key a UI element acted on is CONSUMED (Input.ConsumeKey)
        // before the frame's first script runs, so no script can also read it; a mouse
        // button has no such thing, so a button still held the moment a menu closes
        // would fire the shot that dismissing the menu was meant to be. A trigger the
        // interface was holding is spent, and firing resumes on the next fresh press.
        bool held = Input.IsMouseDown(MouseButton.Left);
        if (Ui.HasFocus)
        {
            _triggerSpent = held;
        }
        else if (!held)
        {
            _triggerSpent = false;
        }

        if (!IsAlive)
        {
            TickDead(deltaTime);
            return;
        }

        if (Ui.HasFocus || _triggerSpent || _sinceFire < FireInterval || !held)
        {
            return;
        }
        Fire();
    }

    /// <summary>
    /// The dead state, in one place: pinned, still, silent, and counting down.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The body is held at its spawn point rather than left where it fell. A corpse is
    /// switched to a sensor by <see cref="Present"/> so nothing can walk into it or
    /// shoot it, but an invisible obstacle-shaped hole in the middle of the arena is
    /// still a thing a player can be surprised by, and the spawn point is where this
    /// player is about to reappear anyway.
    /// </para>
    /// <para>
    /// <b>Nothing else places it.</b> Movement is client-authoritative, so the victim
    /// - and only the victim - moves the victim; a host that also tried would be
    /// writing a transform its owner overwrites 20 ms later, which is the desync this
    /// framework was rebuilt to remove. This method only ever runs under
    /// <see cref="Net.HasAuthority"/>.
    /// </para>
    /// </remarks>
    private void TickDead(float deltaTime)
    {
        if (GetScript<PlayerController>() is { } controller)
        {
            Self.Position = controller.SpawnPoint;
        }
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);

        _respawnIn -= deltaTime;
        if (_respawnIn > 0.0f)
        {
            return;
        }
        _respawnIn = 0.0f;
        Health = MaxHealth;
        _sinceFire = FireInterval;
    }

    /// <summary>Aim at the cursor and ask the host for a projectile.</summary>
    private void Fire()
    {
        Vector3 origin = Self.Position;
        // Exact for the arena's orthographic camera, which is what makes the shot go
        // where the crosshair is rather than where a screen-space approximation of it
        // is.
        Vector3 cursor = Camera.ScreenToWorld(Input.MousePosition);
        Vector2 aim = new(cursor.X - origin.X, cursor.Y - origin.Y);
        float distance = aim.Length();
        if (distance < 0.0001f)
        {
            return; // cursor exactly on the character: no direction to fire in
        }
        aim /= distance;

        _sinceFire = 0.0f;
        _pending.Enqueue(new PendingShot(aim, 0.0f));
        Vector2 muzzle = new(origin.X + aim.X * MuzzleOffset, origin.Y + aim.Y * MuzzleOffset);
        Net.CallServer(Self, nameof(RequestFire), $"{Fmt(muzzle.X)}|{Fmt(muzzle.Y)}");
    }

    private void AgePendingShots(float deltaTime)
    {
        int count = _pending.Count;
        for (int i = 0; i < count; i++)
        {
            PendingShot shot = _pending.Dequeue().Older(deltaTime);
            if (shot.Age <= PendingShotSeconds)
            {
                _pending.Enqueue(shot);
            }
        }
    }

    /// <summary>
    /// The direction of the oldest shot this peer has asked for and not yet been given
    /// a projectile for, claimed by that projectile. Falls back to where this player is
    /// aiming NOW, so a projectile is never left without a direction.
    /// </summary>
    internal Vector2 TakeShotDirection()
    {
        if (_pending.Count > 0)
        {
            return _pending.Dequeue().Direction;
        }
        Log.Warn("[Whisper] PlayerCombat: a projectile arrived with no shot waiting for it; aiming it at the cursor");
        Vector3 origin = Self.Position;
        Vector3 cursor = Camera.ScreenToWorld(Input.MousePosition);
        Vector2 aim = new(cursor.X - origin.X, cursor.Y - origin.Y);
        float distance = aim.Length();
        return distance < 0.0001f ? new Vector2(1.0f, 0.0f) : aim / distance;
    }

    // ── Host ────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Owner -> host: "I fired, from here." The host creates the projectile, owned by
    /// the connection that asked for it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Addressed to the shooter's own player because the host refuses a server RPC
    /// aimed at anything the sender does not own - the same rule that puts
    /// <see cref="ChatBox.SendChat"/> on the player prefab.
    /// </para>
    /// <para>
    /// <b>The rate limit is here as well as on the shooter</b>, and the two are not
    /// redundant. The shooter's gate is the one that runs in the normal path; this one
    /// is what a modified client meets. It is deliberately loose (see
    /// <see cref="HostRateTolerance"/>) - the host is measuring arrival times across a
    /// link, not a trigger being pulled.
    /// </para>
    /// </remarks>
    /// <param name="payload">"x|y" - where the projectile starts, in world space.</param>
    [NetRpc(NetRpcTarget.Server)]
    public void RequestFire(string payload)
    {
        if (!IsAlive)
        {
            return; // the host's own replicated copy says this player is down
        }

        float now = Time.TotalTime;
        if (now - _lastAcceptedFire < FireInterval * HostRateTolerance)
        {
            Log.Warn($"[Whisper] PlayerCombat: refusing a shot from connection {Net.OwnerOf(Self)} - faster than the fire rate");
            return;
        }

        string[] parts = payload.Split('|');
        if (parts.Length != 2
            || !float.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out float x)
            || !float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out float y))
        {
            return;
        }

        Vector3 here = Self.Position;
        if (new Vector2(x - here.X, y - here.Y).Length() > MaxMuzzleDistance)
        {
            Log.Warn($"[Whisper] PlayerCombat: refusing a shot from connection {Net.OwnerOf(Self)} - muzzle is not near the shooter");
            return;
        }

        _lastAcceptedFire = now;
        Net.Spawn(ProjectilePrefab, new Vector3(x, y, 0.0f), Net.OwnerOf(Self));
    }

    /// <summary>
    /// Victim -> host: "that shot killed me, and this is who fired it." The host hands
    /// the credit to the killer's own peer, and tells everybody.
    /// </summary>
    /// <remarks>
    /// The victim cannot address the killer directly: a server RPC is only accepted on
    /// an entity the sender owns, so it says this on ITSELF and the host does the rest.
    /// </remarks>
    /// <param name="killerConnection">The connection that owns the killer's player.</param>
    [NetRpc(NetRpcTarget.Server)]
    public void ReportKill(string killerConnection)
    {
        string victimName = Net.GetPlayerName(Self);
        Entity killer = uint.TryParse(killerConnection, NumberStyles.Integer, CultureInfo.InvariantCulture, out uint connection)
            ? PlayerOwnedBy(connection)
            : default;

        if (!killer.IsValid || killer == Self)
        {
            // The killer left between the shot and the death. The death still counted -
            // the victim has already added it to itself - so say so and stop.
            ChatBox.Announce(Self, $"{victimName} died");
            return;
        }

        // Client-targeted, so it runs on the killer's own peer and nowhere else: only
        // that peer may add to that player's score.
        Net.Call(killer, nameof(AwardKill));
        ChatBox.Announce(Self, $"{Net.GetPlayerName(killer)} fragged {victimName}");
    }

    // ── Owner, on the far end of the host ───────────────────────────────────────

    /// <summary>
    /// Host -> this player's owner: take a hit. Runs on the one peer entitled to
    /// change this player's health, and replication carries the result everywhere.
    /// </summary>
    /// <param name="killerConnection">The connection that owns the shooter.</param>
    [NetRpc(NetRpcTarget.Client)]
    public void ApplyHit(string killerConnection)
    {
        if (!IsAlive)
        {
            return; // already down; a second hit in the same volley does nothing
        }
        Health = System.Math.Max(0, Health - Damage);
        if (IsAlive)
        {
            return;
        }

        // Dead. Both of these are this player's own facts, written here on this
        // player's own machine.
        Deaths++;
        _respawnIn = RespawnSeconds;
        _pending.Clear();
        Net.CallServer(Self, nameof(ReportKill), killerConnection);
    }

    /// <summary>Host -> this player's owner: you landed a killing blow.</summary>
    [NetRpc(NetRpcTarget.Client)]
    public void AwardKill() => Kills++;

    // ── Presentation, on every peer ─────────────────────────────────────────────

    /// <summary>
    /// Show what the replicated health says. Runs on every peer, including the owner,
    /// so a dead player looks and behaves identically everywhere.
    /// </summary>
    private void Present()
    {
        if (Health != _shownHealth)
        {
            _shownHealth = Health;
            // The only readout there is of a replicated SCRIPT field: scene.get_component
            // refuses script components, so without this line a networking testbed has no
            // way to watch health land on a remote peer. Logged on change only.
            Log.Info($"[Whisper] health {Health} on '{Net.GetPlayerName(Self)}' (owner {Net.OwnerOf(Self)})");
        }

        bool alive = IsAlive;
        if (_aliveApplied && alive == _shownAlive)
        {
            return;
        }
        _aliveApplied = true;
        _shownAlive = alive;

        SpriteRenderer.SetVisible(Self, alive);
        // A sensor is invisible to casts as well as to contacts, so switching the
        // corpse to one is what makes "cannot be shot again" true in the physics
        // rather than only in Projectile's explicit liveness test - and stops a dead
        // body blocking a living one. Latched, because this rebuilds the fixture.
        Physics2D.SetTrigger(Self, !alive);
    }

    // ── Shared lookups ──────────────────────────────────────────────────────────

    /// <summary>The player entity <paramref name="connection"/> owns, or an invalid
    /// entity if that connection has none here.</summary>
    /// <remarks>
    /// A connection id is the only name for a player that means the same thing on
    /// every peer - entity ids do not travel - so it is what the hit and kill messages
    /// carry, and this is the one place that turns one back into an entity.
    /// </remarks>
    internal static Entity PlayerOwnedBy(uint connection)
    {
        foreach (Entity player in Net.Players)
        {
            if (Net.OwnerOf(player) == connection)
            {
                return player;
            }
        }
        return default;
    }

    /// <summary>Round-trip-safe float formatting: the receiving peer parses this with
    /// the same invariant culture, whatever locale either machine runs in.</summary>
    private static string Fmt(float value) => value.ToString("R", CultureInfo.InvariantCulture);
}
