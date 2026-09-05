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

    /// <summary>How many seconds of landed hits one shooter may have bunched up at
    /// once before the host's hit pace gate catches up with them.</summary>
    /// <remarks>
    /// Kept on the shooter, not read off <see cref="Projectile"/>, because the gate
    /// bills the shooter: it should say "one full flight's worth of projectiles",
    /// which is Projectile.MaxLifetime's default. Tuning them apart only loosens or
    /// tightens the burst, never the sustained rate - that is the fire interval
    /// above.
    /// </remarks>
    public float HostHitBurstSeconds = 1.0f;

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

    /// <summary>Requests this peer has sent the host for a projectile and not yet
    /// been handed a projectile for, in order. See <see cref="PredictedSpawnQueue{T}"/>
    /// for why order is exact and why an unclaimed entry expires.</summary>
    private readonly PredictedSpawnQueue<Vector2> _pendingShots = new();

    /// <summary>How long an unclaimed pending shot survives - the host refused the
    /// request, or its Spawn reply was lost - before its predicted echo is retired
    /// rather than left flying forever. Copied onto <see cref="_pendingShots"/>
    /// every tick, so tuning this live takes effect immediately.</summary>
    public float PendingShotSeconds = 1.0f;

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

    // Host-side validation state (the methods that use it sit after ReportKill):
    // Everything below lives on the HOST's copy of a player and exists because a
    // server RPC says whatever its sender wants it to say. The host cannot make the
    // shooter honest - it never re-simulates the shot - but it can stop taking the
    // report's word for pace, proof and identity. Offline none of it runs: there is
    // no other peer to disbelieve, and the one peer there is owns every player, so
    // HasAuthority paths are the only paths.

    // Per-shooter hit pace as a lazily refilled token bucket. The fire gate above
    // lets at most one projectile leave per FireInterval * HostRateTolerance, and a
    // projectile is airborne for HostHitBurstSeconds' worth of them at once - plus
    // one, for several honest hits arriving in the same burst. Refill rate equals
    // the fire rate exactly, which is what makes "hits land no faster than the gun
    // can fire" true for a client that skips its own trigger discipline.

    private float _hitTokens;
    private float _hitTokensAt = float.NegativeInfinity;

    // The host's own account of this player's health, decremented when the host
    // ACCEPTS a hit - never read back off the wire. A replicated script field and
    // an RPC travel on the same reliable channel, but the RPC is queued the moment
    // ApplyHit returns while the field waits for the send pass, so a ReportKill can
    // arrive while the replicated Health still reads one hit above zero. This
    // ledger has no such race: it is written before ApplyHit is sent, which is
    // strictly before the victim can be hurt, let alone die and report it.
    private int _hostHealthShadow = 100;

    // The death the host can account for: when the ledger crosses zero, who fired
    // the finishing hit (connection and entity - the entity is what catches a
    // connection id that has been reused by a different, later joiner), and whether
    // that death still needs its kill credited.
    private float _hostDownAt;
    private bool _hostKillOpen;
    private uint _hostLethalConnection;
    private Entity _hostLethalEntity;

    // One warning per player per session for refused host traffic. A flood that is
    // being dropped must not become a second flood in the log.
    private bool _hostTrafficWarned;

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
        // The host's ledger starts at spawn health. A field initializer cannot say
        // MaxHealth (instance field), and a prefab that retunes it must not leave the
        // host counting a different body than the owner applies damage to.
        _hostHealthShadow = MaxHealth;
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
        _pendingShots.TimeoutSeconds = PendingShotSeconds;
        _pendingShots.Age(deltaTime);

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
        Vector2 muzzle = new(origin.X + aim.X * MuzzleOffset, origin.Y + aim.Y * MuzzleOffset);
        Entity ghost = Net.SpawnPredicted(ProjectilePrefab, new Vector3(muzzle.X, muzzle.Y, 0.0f), (Self, aim));
        _pendingShots.Expect(aim, ghost);
        Net.CallServer(Self, nameof(RequestFire), $"{Fmt(muzzle.X)}|{Fmt(muzzle.Y)}");
    }

    /// <summary>
    /// The direction of the oldest shot this peer has asked for and not yet been given
    /// a projectile for, claimed by that projectile. Falls back to where this player is
    /// aiming NOW, so a projectile is never left without a direction.
    /// </summary>
    internal Vector2 TakeShotDirection()
    {
        if (_pendingShots.TryReconcile(out Vector2 direction))
        {
            return direction;
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
            NoteRefusedHostTraffic("shots faster than the fire rate");
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
            NoteRefusedHostTraffic("a muzzle nowhere near the shooter");
            return;
        }

        _lastAcceptedFire = now;
        Net.Spawn(ProjectilePrefab, new Vector3(x, y, 0.0f), Net.OwnerOf(Self));
    }

    /// <summary>
    /// Victim -> host: "that shot killed me, and this is who fired it." The host
    /// decides whether to believe it, and only then hands the credit to the killer's
    /// own peer and tells everybody.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The victim cannot address the killer directly: a server RPC is only accepted on
    /// an entity the sender owns, so it says this on ITSELF and the host does the rest.
    /// </para>
    /// <para>
    /// <b>The host carries the kill.</b> A ReportKill names any connection it likes,
    /// so the death is checked against the host's own ledger of accepted hits (the
    /// killer must be the one the ledger recorded as landing the finishing blow) and
    /// the credit is one-shot per death. The victim still applies the death to itself
    /// - Deaths and the respawn timer are the victim's own facts, as they have always
    /// been - but it can no longer award kills, frame a name into every transcript,
    /// or announce anything the host cannot account for.
    /// </para>
    /// </remarks>
    /// <param name="killerConnection">The connection that owns the killer's player.</param>
    [NetRpc(NetRpcTarget.Server)]
    public void ReportKill(string killerConnection)
    {
        if (!uint.TryParse(killerConnection, NumberStyles.Integer, CultureInfo.InvariantCulture, out uint killerId)
            || !_hostKillOpen
            || killerId != _hostLethalConnection)
        {
            // Not a death the host can account for: nobody it recorded as lethal hit
            // this player, or this death has already been credited. Silent - a
            // refusal line per attempt is exactly the announcement flood this gate
            // exists to stop.
            NoteRefusedHostTraffic("a kill report the host cannot account for");
            return;
        }
        _hostKillOpen = false; // one credit per death, whatever arrives after it

        string victimName = Net.GetPlayerName(Self);
        Entity killer = PlayerOwnedBy(killerId);

        if (!killer.IsValid || killer != _hostLethalEntity)
        {
            // The killer left between the shot and the death - or the connection id
            // now names a different, later joiner; connection ids are reused after a
            // disconnect/rejoin cycle. The ENTITY comparison is what tells those
            // apart from an honest credit: a slot handed to a new player is a new
            // entity, so a reused id fails it where a merely-departed one also does,
            // and neither gets the kill. The death is real either way - the ledger
            // proved it above - so say so and stop.
            ChatBox.Announce(Self, $"{victimName} died");
            return;
        }

        // Client-targeted, so it runs on the killer's own peer and nowhere else: only
        // that peer may add to that player's score.
        Net.Call(killer, nameof(AwardKill));
        ChatBox.Announce(Self, $"{Net.GetPlayerName(killer)} fragged {victimName}");
    }

    // ── Host-side validation ────────────────────────────────────────────────────

    /// <summary>
    /// Host-side: charge one landed hit against this shooter's pace. True to accept,
    /// false when hits are arriving faster than the fire gate lets projectiles leave.
    /// </summary>
    /// <remarks>
    /// Runs on the host's copy of the SHOOTER's player, called from
    /// <see cref="Projectile.ReportHit"/> after the per-projectile checks. Refusing
    /// here is what bounds a modified client that creates no bogus projectiles but
    /// replays hit reports: sustained, it can land hits no faster than the weapon's
    /// own fire rate.
    /// </remarks>
    internal bool TryAcceptHostHit(float now)
    {
        float interval = FireInterval * HostRateTolerance;
        float burst = (float)((int)System.Math.Ceiling(HostHitBurstSeconds / interval) + 1);
        _hitTokens = System.Math.Min(burst, _hitTokens + (now - _hitTokensAt) / interval);
        _hitTokensAt = now;
        if (_hitTokens < 1.0f)
        {
            return false;
        }
        _hitTokens -= 1.0f;
        return true;
    }

    /// <summary>
    /// Host-side: record that a hit the host accepted is about to take
    /// <see cref="Damage"/> off this player, and remember who fired the one that,
    /// by the host's account, finishes it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Runs on the host's copy of the VICTIM's player, from
    /// <see cref="Projectile.ReportHit"/>, before ApplyHit is sent. Because the
    /// owner applies every hit the host accepted - and only those - in the order
    /// the host sent them, this ledger tracks the victim's true health exactly;
    /// <see cref="ReportKill"/> trusts it rather than the replicated
    /// <see cref="Health"/>, which can still read one hit stale when a kill report
    /// overtakes its own field update.
    /// </para>
    /// <para>
    /// The respawn branch reconstructs itself from evidence: a hit can only LAND on
    /// a living player (the owner's ApplyHit no-ops on a corpse), so a hit arriving
    /// while the ledger says down is either a late report against the corpse -
    /// ignored, exactly as the owner ignores it - or, once the respawn wait has
    /// passed, proof the player is back up, and the ledger reopens at full health
    /// before charging it.
    /// </para>
    /// </remarks>
    internal void NoteHostHit(uint shooter, float now)
    {
        if (_hostHealthShadow <= 0)
        {
            if (now - _hostDownAt < RespawnSeconds)
            {
                return; // against the corpse: the owner's ApplyHit no-ops too
            }
            _hostHealthShadow = MaxHealth; // the victim must be back up: it just took a hit
        }
        _hostHealthShadow -= Damage;
        if (_hostHealthShadow > 0)
        {
            return;
        }
        _hostDownAt = now;
        _hostKillOpen = true;
        _hostLethalConnection = shooter;
        _hostLethalEntity = PlayerOwnedBy(shooter);
    }

    /// <summary>Host-side: note that traffic from this player was refused by one of
    /// the host's gates. Logged once per player per session.</summary>
    /// <remarks>
    /// Once, because the refusals themselves arrive at wire speed: a warning line
    /// per dropped packet is a second flood the host pays for, in its log this time.
    /// </remarks>
    internal void NoteRefusedHostTraffic(string why)
    {
        if (_hostTrafficWarned)
        {
            return;
        }
        _hostTrafficWarned = true;
        Log.Warn($"[Whisper] PlayerCombat: dropping traffic from connection {Net.OwnerOf(Self)} - {why}; further drops are silent");
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
        _pendingShots.Clear();
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
