using System.Globalization;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One shot in flight: a replicated entity owned by the peer that fired it, moved and
/// resolved by that peer, and destroyed by the host.
/// </summary>
/// <remarks>
/// <para>
/// <b>The shooter owns it, so the shooter decides everything about it</b> - where it
/// is this frame, what it hit, and when it is done. That is the same
/// owner-is-authoritative rule the players themselves follow, and it is why a shot
/// registers on the shooter's screen with no round trip once the projectile exists.
/// Every other peer's copy is driven purely by replication, exactly like a remote
/// player: it never simulates, never casts, and never reports anything. What the
/// shooter does NOT decide is what the host will believe: <see cref="ReportHit"/>
/// bounds each projectile to one accepted resolution, each claimed victim to
/// somewhere the shot could have reached, and each shooter to the weapon's own
/// pace - the host's copy of the same rules, for the copies that never simulate.
/// </para>
/// <para>
/// <b>Bounding the flight is the important part.</b> A projectile that misses and is
/// not cleaned up leaks on EVERY peer, so there are two independent limits and they
/// do not depend on each other:
/// </para>
/// <list type="number">
/// <item>The owner ends it - on a wall, on a player, or at
/// <see cref="MaxLifetime"/> - and asks the host to despawn it.</item>
/// <item><b>The host destroys any projectile older than
/// <see cref="MaxLifetime"/> + <see cref="HostGraceSeconds"/> whatever its owner
/// thinks</b>, which is what covers an owner that stopped simulating, hung, or was
/// never listening. The host's clock for this starts when the host built the entity,
/// so it needs nothing from anybody.</item>
/// </list>
/// <para>
/// A third bound comes free: the framework destroys everything a dropped connection
/// owned, so a shooter that disconnects mid-flight takes its projectiles with it.
/// </para>
/// </remarks>
public sealed class Projectile : EntityScript
{
    /// <summary>World units per second.</summary>
    public float Speed = 22.0f;

    /// <summary>Seconds a projectile flies before giving up. 22 x 1.0 is 22 units,
    /// comfortably past the far wall of an 18-unit arena from any spawn.</summary>
    public float MaxLifetime = 1.0f;

    /// <summary>How much longer than <see cref="MaxLifetime"/> the host lets a
    /// projectile live before destroying it regardless of its owner.</summary>
    /// <remarks>
    /// One generous round trip. The owner's own despawn request normally lands long
    /// before this; the grace is only so the two bounds do not race and produce a
    /// double despawn on a healthy link.
    /// </remarks>
    public float HostGraceSeconds = 0.75f;

    /// <summary>Sweep radius, roughly the drawn sprite's - the step cast is extended
    /// by this so a shot grazing a body still connects.</summary>
    public float Radius = 0.18f;

    /// <summary>How much further from the victim position the host judges the claim
    /// against than the projectile's own flight can explain may be before the host
    /// refuses the hit.</summary>
    /// <remarks>
    /// Not a physics check - a bound, like <c>PlayerCombat.MaxMuzzleDistance</c>.
    /// <see cref="ReportHit"/> judges the claim against where <see cref="Net.TryRewind"/>
    /// reconstructs the victim as having been on the SHOOTER's own screen, which already
    /// absorbs the round trip's worth of replication lag this slack used to cover by
    /// itself (it used to be 4 units for exactly that reason). What is left for this to
    /// cover is narrower: the victim's own collider half-width - a shot grazing the edge
    /// of a body should still connect - and the residual error in TryRewind's own
    /// estimate, which is a capped approximation, not a measurement (see
    /// <c>NetRewind.hpp</c>), and is deliberately biased toward under- rather than
    /// over-compensating the shooter's claim. When TryRewind has no history to rewind
    /// (the victim is host-owned, in which case its live position already IS the
    /// ground truth with no lag to compensate for - see <see cref="ReportHit"/>) this
    /// is compared against the victim's LIVE position instead, which is exactly as
    /// accurate as the rewound case for a host-owned victim, not a weaker fallback.
    /// </remarks>
    public float HitClaimSlack = 1.0f;

    private Vector2 _direction;
    private Entity _shooter;
    private float _age;
    private bool _spent;

    // Host-side resolution latch. The owner's _spent never travels - it lives only
    // on the shooter's machine, and the host's copy of a client's projectile never
    // simulates - so the host keeps its own: ONE ReportHit accepted per projectile,
    // ever. Without it, a modified client could replay hit reports on one entity
    // and drain any victim's health at wire speed; with it, the most one projectile
    // can ever cost its claimed victim is one PlayerCombat.Damage.
    private bool _hostResolved;

    // Where this copy first saw the projectile - on the host, the muzzle position
    // RequestFire already validated. Recorded in OnAttach, before replication can
    // have moved anything, so it is exact; only the host's copy ever reads it.
    private Vector3 _hostSpawn;

    /// <summary>
    /// True for a local, display-only echo of this peer's own shot - see
    /// <see cref="Net.SpawnPredicted{T}"/> - spawned instantly instead of waiting
    /// on <see cref="PlayerCombat.RequestFire"/>'s round trip. The framework
    /// already keeps it from damaging anyone or reporting anything real - it
    /// carries no network id, so <see cref="Net.CallServer"/> against it is
    /// refused exactly like it would be for any other unreplicated entity (see
    /// <see cref="Resolve"/> and <see cref="Net.SpawnPredicted{T}"/>'s remarks).
    /// This flag is only for the one thing the framework cannot know on its own:
    /// how this entity cleans ITSELF up when nobody is ever going to send a
    /// despawn for it - see <see cref="Expire"/>.
    /// </summary>
    internal bool IsPredicted { get; private set; }

    /// <inheritdoc/>
    public override void OnAttach()
    {
        _hostSpawn = Self.Position;

        if (Net.TryTakePredictedSpawn(out (Entity Shooter, Vector2 Direction) predicted))
        {
            // A local, display-only echo of this peer's own shot, shown instantly
            // instead of waiting on RequestFire's round trip - see IsPredicted for
            // what that turns off. The shooter/direction come from the side-channel
            // payload rather than Net.OwnerOf(Self): Net.SpawnPredicted's fix-up
            // (see its remarks) only runs once Scene.Instantiate RETURNS, so for
            // the rest of THIS call Self still carries the "bullet" prefab's own
            // baked NetworkIdentity and would resolve to the HOST, not whoever
            // actually fired this.
            _shooter = predicted.Shooter;
            _direction = predicted.Direction;
            IsPredicted = true;
        }
        else
        {
            // Resolved on every peer: the tint below needs it, and the owner needs
            // it as the one body its own shot may never report a hit on.
            _shooter = PlayerCombat.PlayerOwnedBy(Net.OwnerOf(Self));
            if (Net.HasAuthority(Self) && _shooter.GetScript<PlayerCombat>() is { } combat)
            {
                // The Spawn message carries a prefab and a position; the direction
                // is recovered here from the shot this projectile belongs to. See
                // PlayerCombat's pending-shot queue for why that is exact.
                _direction = combat.TakeShotDirection();
            }
        }

        // Wearing the shooter's colour, from the same PlayerPalette entry as that
        // player's character, name tag and roster row - so who is shooting is
        // legible without reading anything.
        if (_shooter.GetScript<NetPlayerSync>() is { } sync)
        {
            SpriteRenderer.SetTint(Self, sync.Color);
        }
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        _age += deltaTime;

        // THE BOUND. Unconditional, host-side, and independent of whatever the owner
        // believes or is still able to say.
        if (Net.IsHost && _age > MaxLifetime + HostGraceSeconds)
        {
            Log.Warn($"[Whisper] Projectile: reaping a projectile from connection {Net.OwnerOf(Self)} that outlived its owner's despawn");
            Net.Despawn(Self);
            return;
        }

        // By now Net.SpawnPredicted's native fix-up has already run - it happens
        // the instant Scene.Instantiate returns, well before this, the entity's
        // first OnUpdate - so Net.HasAuthority(Self) already reads true for a
        // predicted echo exactly like it does for the real thing; no separate
        // test is needed here any more.
        if (_spent || !Net.HasAuthority(Self) || Time.IsPaused)
        {
            return;
        }

        if (_direction == Vector2.Zero)
        {
            Expire(); // no direction to fly in; do not leave it sitting in the world
            return;
        }

        Vector3 position = Self.Position;
        Vector2 from = new(position.X, position.Y);
        float step = Speed * deltaTime;

        // A body this projectile is already INSIDE, which the cast below cannot see:
        // a ray whose origin lies within a shape reports no hit against it. At contact
        // range the muzzle is inside the target - MuzzleOffset is 0.9 and a character
        // is 1.0 wide - so without this a shot pressed against somebody passes straight
        // through them and every point-blank hit is lost.
        Entity touching = OverlappedVictim(from);
        if (touching.IsValid)
        {
            Resolve(touching);
            return;
        }

        // ONE query for walls and players both. Casting the step about to be taken -
        // rather than testing an overlap after taking it - is what stops a fast
        // projectile stepping straight through a body between two frames.
        RaycastHit2D hit = Physics2D.Raycast(from, _direction, step + Radius);
        if (hit.DidHit && hit.Entity != _shooter)
        {
            Resolve(hit.Entity);
            return;
        }

        Self.Position = new Vector3(from.X + _direction.X * step, from.Y + _direction.Y * step, position.Z);
        if (_age >= MaxLifetime)
        {
            Expire();
        }
    }

    /// <summary>
    /// A living player - never the shooter - whose body already contains this
    /// projectile, or an invalid entity.
    /// </summary>
    /// <remarks>
    /// The shooter is skipped rather than resolved against, exactly as it is in the
    /// cast: a muzzle that clips its own owner must let the shot THROUGH, not end it.
    /// Corpses are skipped for the same reason they are unhittable everywhere else -
    /// they are sensors by then, and a shot should pass over one rather than stop on
    /// it. Scenery is left to the cast, which handles it correctly from outside and
    /// is the only one of the two that can stop a shot at the wall's face.
    /// </remarks>
    private Entity OverlappedVictim(Vector2 at)
    {
        foreach (Entity candidate in Physics2D.OverlapCircle(at, Radius))
        {
            if (candidate != _shooter && candidate.GetScript<PlayerCombat>() is { IsAlive: true })
            {
                return candidate;
            }
        }
        return default;
    }

    /// <summary>What the projectile struck: a player worth damaging, or scenery.
    /// Either way this shot is over.</summary>
    private void Resolve(Entity struck)
    {
        // Alive is read off the victim's REPLICATED health, so a corpse cannot be
        // shot again - and its collider is a sensor by then, so this branch is
        // usually not even reached. No IsPredicted test is needed here any more: a
        // predicted echo carries no network id (see Net.SpawnPredicted), so
        // Net.CallServer below is already refused for it exactly like it would be
        // for any other unreplicated entity, rather than something this method has
        // to remember to check.
        if (struck != _shooter && struck.GetScript<PlayerCombat>() is { IsAlive: true })
        {
            // Addressed to THIS projectile, which this peer owns. The host refuses a
            // server RPC aimed at anything the sender does not own, so the hit cannot
            // be sent on the victim's own entity however natural that would read.
            Net.CallServer(Self, nameof(ReportHit), Net.OwnerOf(struck).ToString(CultureInfo.InvariantCulture));
        }
        Expire();
    }

    /// <summary>Stop simulating and destroy this - asking the host first, unless
    /// this is a predicted echo the host has never heard of.</summary>
    /// <remarks>
    /// Hidden here rather than waiting for the despawn to come back, so the shot
    /// disappears on the shooter's screen the instant it lands. The real entity is
    /// destroyed by the host on every peer at once, through one path, which is what
    /// makes "the entity count returns to where it started" mean something; a
    /// predicted echo has no such path to take - it is purely local - so it destroys
    /// itself directly instead.
    /// </remarks>
    private void Expire()
    {
        _spent = true;
        SpriteRenderer.SetVisible(Self, false);
        if (IsPredicted)
        {
            Self.Destroy();
            return;
        }
        Net.CallServer(Self, nameof(RequestDespawn));
    }

    /// <summary>Owner -> host: this projectile is finished.</summary>
    [NetRpc(NetRpcTarget.Server)]
    public void RequestDespawn() => Net.Despawn(Self);

    /// <summary>
    /// Owner -> host: this projectile hit the player owned by
    /// <paramref name="victimConnection"/>.
    /// </summary>
    /// <remarks>
    /// The host does not re-simulate the shot - the shooter is authoritative for its
    /// own projectile, which is the accepted trade of this design - but it does check
    /// the things that are its own to know: that the named player exists, is not the
    /// shooter, and is still alive; that this projectile has not already resolved
    /// (one hit per shot, the same rule the owner's <c>_spent</c> enforces on the
    /// shooter's machine); that the claimed victim was, from the SHOOTER's own point
    /// of view (<see cref="Net.TryRewind"/> - see its remarks and <c>NetRewind.hpp</c>
    /// for the honesty boundary on the rewind time), somewhere the projectile could
    /// physically have reached; and that this shooter is not landing hits faster than
    /// the fire gate lets projectiles leave the barrel. What the host still takes on
    /// trust is the geometry of the hit itself - without host-side re-simulation there
    /// is no way to know the shot was ever aimed at the victim, only that it could have
    /// been. <b>The accepted cost of the rewind:</b> a victim can be judged as having
    /// been hit up to 300ms (<c>kMaxRewindSeconds</c>) after their own screen already
    /// showed them clear of it - "shot behind cover" - which is the trade every
    /// lag-compensated shooter game makes, bounded to 300ms of the past rather than
    /// an unbounded one.
    /// </remarks>
    [NetRpc(NetRpcTarget.Server)]
    public void ReportHit(string victimConnection)
    {
        if (!uint.TryParse(victimConnection, NumberStyles.Integer, CultureInfo.InvariantCulture, out uint connection))
        {
            return;
        }
        uint shooter = Net.OwnerOf(Self);
        if (connection == shooter)
        {
            return; // nobody shoots themselves, whatever a peer claims
        }
        Entity victim = PlayerCombat.PlayerOwnedBy(connection);
        if (victim.GetScript<PlayerCombat>() is not { IsAlive: true } victimCombat)
        {
            return; // gone, or already down
        }
        Entity shooterPlayer = PlayerCombat.PlayerOwnedBy(shooter);
        if (shooterPlayer.GetScript<PlayerCombat>() is not { } shooterCombat)
        {
            return; // the shooter's connection is gone; there is nobody to bill this hit to
        }

        // The host's own bounds. Each closes one cheat the checks above do not:
        // replaying one resolution against many victims or many times, claiming a
        // victim further away than the shot has had time to travel, and pacing hit
        // reports faster than the weapon fires. Offline none of this is reachable -
        // the self-hit test above has already returned, because the one peer there
        // is owns every player.
        float now = Time.TotalTime;
        if (_hostResolved)
        {
            shooterCombat.NoteRefusedHostTraffic("a projectile cannot hit twice");
            return;
        }
        if (_age > MaxLifetime + HostGraceSeconds)
        {
            return; // past any flight its owner could still be reporting; the reap is due
        }
        // Judged against where the shooter's OWN screen showed this victim, not
        // where the host's copy of them is right now - that is the entire point of
        // Net.TryRewind (see its remarks and NetRewind.hpp for the honesty
        // boundary: the rewind time comes only from this host's own estimate of
        // the shooter's view delay, hard-capped, never a number either peer
        // supplies). TryRewind has nothing to rewind - and this falls back to the
        // LIVE position, exactly as before TryRewind existed - when the victim is
        // host-owned (its live position already IS this host's own truth, with no
        // network leg to compensate for), not yet interpolated (just spawned), or
        // not replicated at all; none of those are a weaker check, they are cases
        // where "now" and "as the shooter saw it" already agree.
        Vector3 victimPosition = Net.TryRewind(victim, shooter, out Vector3 rewound, out _, out _)
            ? rewound
            : victim.Position;
        Vector2 fromSpawn = new(victimPosition.X - _hostSpawn.X, victimPosition.Y - _hostSpawn.Y);
        float reach = Speed * System.Math.Min(_age, MaxLifetime) + Radius + HitClaimSlack;
        if (fromSpawn.Length() > reach)
        {
            shooterCombat.NoteRefusedHostTraffic("a claimed victim out of the projectile's reach");
            return;
        }
        if (!shooterCombat.TryAcceptHostHit(now))
        {
            return; // hits landing faster than the fire gate lets shots leave
        }

        _hostResolved = true;
        // The victim's ledger entry is recorded BEFORE ApplyHit is sent, which is
        // what makes the host's account of who killed whom immune to the race
        // between an RPC and the health field it changes - see NoteHostHit.
        victimCombat.NoteHostHit(shooter, now);
        // Client-targeted, so it lands on the victim's own peer: the only one allowed
        // to change that player's health.
        Net.Call(victim, nameof(PlayerCombat.ApplyHit), shooter.ToString(CultureInfo.InvariantCulture));
    }
}
