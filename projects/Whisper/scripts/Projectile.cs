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
/// player: it never simulates, never casts, and never reports anything.
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

    private Vector2 _direction;
    private Entity _shooter;
    private float _age;
    private bool _spent;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        // Resolved on every peer: the tint below needs it, and the owner needs it as
        // the one body its own shot may never report a hit on.
        _shooter = PlayerCombat.PlayerOwnedBy(Net.OwnerOf(Self));

        // Wearing the shooter's colour, from the same PlayerPalette entry as that
        // player's character, name tag and roster row - so who is shooting is legible
        // without reading anything.
        if (_shooter.GetScript<NetPlayerSync>() is { } sync)
        {
            SpriteRenderer.SetTint(Self, sync.Color);
        }

        if (Net.HasAuthority(Self) && _shooter.GetScript<PlayerCombat>() is { } combat)
        {
            // The Spawn message carries a prefab and a position; the direction is
            // recovered here from the shot this projectile belongs to. See
            // PlayerCombat's pending-shot queue for why that is exact.
            _direction = combat.TakeShotDirection();
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
        // usually not even reached.
        if (struck != _shooter && struck.GetScript<PlayerCombat>() is { IsAlive: true })
        {
            // Addressed to THIS projectile, which this peer owns. The host refuses a
            // server RPC aimed at anything the sender does not own, so the hit cannot
            // be sent on the victim's own entity however natural that would read.
            Net.CallServer(Self, nameof(ReportHit), Net.OwnerOf(struck).ToString(CultureInfo.InvariantCulture));
        }
        Expire();
    }

    /// <summary>Stop simulating and ask the host to destroy this.</summary>
    /// <remarks>
    /// Hidden here rather than waiting for the despawn to come back, so the shot
    /// disappears on the shooter's screen the instant it lands. The entity itself is
    /// destroyed by the host on every peer at once, through one path, which is what
    /// makes "the entity count returns to where it started" mean something.
    /// </remarks>
    private void Expire()
    {
        _spent = true;
        SpriteRenderer.SetVisible(Self, false);
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
    /// shooter, and is still alive.
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
        if (victim.GetScript<PlayerCombat>() is not { IsAlive: true })
        {
            return; // gone, or already down
        }
        // Client-targeted, so it lands on the victim's own peer: the only one allowed
        // to change that player's health.
        Net.Call(victim, nameof(PlayerCombat.ApplyHit), shooter.ToString(CultureInfo.InvariantCulture));
    }
}
