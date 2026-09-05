using System.Collections.Generic;
using System.Numerics;

namespace AetherCore;

public static partial class Net
{
    // Hand-off for the payload a predicted spawn's own OnAttach needs, exactly the
    // shape Whisper's PlayerCombat/Projectile pair had to invent by hand (a static
    // shooter/direction pair) - generalized to any one payload of any type. STATIC
    // and not per-call state: Scene.Instantiate runs OnAttach synchronously, this
    // engine drives one script update at a time, and a script only ever predicts
    // its own spawns, so at most one predicted attach is ever in flight when this
    // is read. Set immediately before Scene.Instantiate and always cleared before
    // SpawnPredicted returns, so it can only ever be read by the OnAttach it was
    // set for - a replicated entity attaches long after this is clear and reads
    // nothing here.
    private static object? s_predictedPayload;
    private static bool s_predictedPending;

    /// <summary>
    /// Spawns a purely local, non-replicated stand-in for an entity this peer is
    /// about to ask the host for, so the result is on screen before the round trip
    /// that creates the authoritative copy completes - immediate feedback for
    /// firing, placing, casting, anything a client requests through a
    /// <see cref="NetRpcTarget.Server"/> call and expects the host to
    /// <see cref="Spawn"/> back. <paramref name="payload"/> reaches the new
    /// entity's own <c>OnAttach</c> through <see cref="TryTakePredictedSpawn{T}"/> -
    /// see that method for why a hand-off is required at all instead of just
    /// reading the entity back.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>Returns an invalid entity, and predicts nothing, unless this peer is a
    /// client.</b> Offline and on the host there is no round trip to hide - the
    /// server call this normally races against already runs locally and
    /// <see cref="Spawn"/>'s result reaches this screen the same frame - so a
    /// prediction on top of that would be the doubled entity this exists to
    /// prevent. A caller does not need to branch on <see cref="IsClient"/> itself;
    /// this already has, and single-player draws exactly one of whatever it spawns.
    /// </para>
    /// <para>
    /// <b>The result can never be confused for the authoritative one.</b> Whatever
    /// <c>NetworkIdentity</c> <paramref name="prefab"/> bakes in - typically owner
    /// 0, which IS the host's connection id - is stripped immediately, before this
    /// returns. With it gone, <see cref="HasAuthority"/> and <see cref="IsOwner"/>
    /// both read TRUE for this entity (this peer is exactly who should be driving
    /// it) and <see cref="OwnerOf"/> reads this peer's own connection id rather
    /// than the host's - no more short-circuiting an authority check by hand. It is
    /// also inert on the wire BY CONSTRUCTION, not by the caller's discipline: with
    /// no network id, <see cref="Call"/>/<see cref="CallServer"/> against it are
    /// refused exactly as they are for any other unreplicated entity, and it is
    /// invisible to every send/receive/relevancy pass. A predicted entity therefore
    /// needs no special-casing anywhere it would otherwise report a hit, apply
    /// damage, or claim a name - it simply cannot reach the host to do so.
    /// </para>
    /// <para>
    /// It is still a locally driven entity, though, so anything it must never do
    /// SPECULATIVELY - award itself a kill, consume a pickup, open a door - has to
    /// be held for reconciliation regardless; this only removes the network-address
    /// half of that problem, not the game logic half.
    /// </para>
    /// </remarks>
    public static Entity SpawnPredicted<T>(string prefab, Vector3 position, T payload)
    {
        if (!IsClient)
        {
            return default;
        }
        s_predictedPayload = payload;
        s_predictedPending = true;
        Entity entity = Scene.Instantiate(prefab, position);
        s_predictedPending = false; // consumed by the new entity's OnAttach; harmless if it was not
        if (entity.IsValid)
        {
            Native.aether_net_mark_predicted(entity.Id);
        }
        return entity;
    }

    /// <summary>
    /// <see cref="SpawnPredicted{T}"/> for a prediction with nothing to hand off
    /// beyond its position - the new entity learns it is predicted from
    /// <see cref="TryTakePredictedSpawn"/> alone.
    /// </summary>
    public static Entity SpawnPredicted(string prefab, Vector3 position)
        => SpawnPredicted<object?>(prefab, position, null);

    /// <summary>
    /// Called from a script's own <c>OnAttach</c>: true, with <paramref name="payload"/>
    /// set, exactly when this attach is for the predicted echo the current
    /// <see cref="SpawnPredicted{T}"/> call just created; false for every other
    /// attach, including the later, authoritative one <see cref="Spawn"/> produces
    /// for the same request.
    /// </summary>
    /// <remarks>
    /// <b>Why this exists instead of the entity just reading its own state back.</b>
    /// The Spawn message that creates the authoritative copy carries a prefab and a
    /// position and nothing else - a game's own request-specific data (which
    /// direction, which target, which loadout) has nowhere on the wire to ride, so
    /// it never reaches that copy's <c>OnAttach</c> either. Both copies therefore
    /// recover it the same way: the real one from whatever request-tracking the
    /// owner script itself kept (see <see cref="PredictedSpawnQueue{T}"/>), and the
    /// predicted one from this - the one hand-off <see cref="SpawnPredicted{T}"/>
    /// leaves for it, because at the moment it attaches there is no request record
    /// to look itself up in yet.
    /// </remarks>
    /// <typeparam name="T">
    /// Must match the type <see cref="SpawnPredicted{T}"/> was called with, or this
    /// returns false the same as a non-predicted attach - there is exactly one
    /// payload waiting, for exactly one type.
    /// </typeparam>
    public static bool TryTakePredictedSpawn<T>(out T payload)
    {
        if (s_predictedPending && s_predictedPayload is T typed)
        {
            payload = typed;
            s_predictedPending = false;
            return true;
        }
        payload = default!;
        return false;
    }

    /// <summary><see cref="TryTakePredictedSpawn{T}"/> for a prediction with no
    /// payload - see <see cref="SpawnPredicted(string,Vector3)"/>.</summary>
    public static bool TryTakePredictedSpawn() => TryTakePredictedSpawn<object?>(out _);
}

/// <summary>
/// Tracks the predicted spawns one owner script has asked the host for and has not
/// yet been handed the authoritative entity for, in request order, and retires
/// whichever ones never get claimed.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why order is exact.</b> A request and the Spawn it produces both travel the
/// reliable ORDERED channel, so this connection's requests reach the host in the
/// order they were made and its spawns come back in the order the host made them.
/// First in, first claimed - <see cref="TryReconcile"/> always takes the oldest.
/// </para>
/// <para>
/// <b>Why entries expire.</b> A host that refuses a request - rate-limited,
/// out of range, whatever the game's own server RPC checks - never sends a Spawn
/// back for it, and an entry nothing ever claims would otherwise pair every later
/// spawn with the wrong request for the rest of the session, and its predicted
/// echo would fly, sit, or idle on screen forever. Call <see cref="Age"/> once per
/// owner tick to bound both.
/// </para>
/// <para>
/// One instance per kind of thing an owner predicts (a weapon, a placeable) -
/// there is one FIFO per wire-ordered request stream, not one for the whole game.
/// </para>
/// </remarks>
public sealed class PredictedSpawnQueue<T>
{
    private readonly struct Pending(T payload, float age, Entity ghost)
    {
        public readonly T Payload = payload;
        public readonly float Age = age;
        public readonly Entity Ghost = ghost;

        public Pending Older(float deltaTime) => new(Payload, Age + deltaTime, Ghost);
    }

    private readonly Queue<Pending> _pending = new();

    /// <summary>How long an unclaimed prediction survives before it is retired as
    /// orphaned. Defaults to a generous one second; a game with its own bound on
    /// how long a request can legitimately take (a projectile's flight time, a
    /// server RPC's own timeout) should usually match that bound here instead.</summary>
    public float TimeoutSeconds { get; set; } = 1.0f;

    /// <summary>How many requests are still waiting on a reply.</summary>
    public int Count => _pending.Count;

    /// <summary>
    /// Record a request this peer just sent to the host, alongside the local,
    /// display-only echo of it - <c>default</c> when <see cref="Net.SpawnPredicted{T}"/>
    /// made none (offline, or hosting - see its remarks), which this queue treats
    /// exactly like a claimed-and-gone ghost: nothing to destroy, either at
    /// reconciliation or on timeout.
    /// </summary>
    public void Expect(T payload, Entity ghost = default) => _pending.Enqueue(new Pending(payload, 0.0f, ghost));

    /// <summary>
    /// Age every outstanding request by <paramref name="deltaTime"/> and retire -
    /// destroying its predicted echo, if it had one - any that have gone unclaimed
    /// past <see cref="TimeoutSeconds"/>. Call this once per owner tick, alongside
    /// whatever else only the owner does.
    /// </summary>
    public void Age(float deltaTime)
    {
        int count = _pending.Count;
        for (int i = 0; i < count; i++)
        {
            Pending entry = _pending.Dequeue().Older(deltaTime);
            if (entry.Age <= TimeoutSeconds)
            {
                _pending.Enqueue(entry);
            }
            else if (entry.Ghost.IsValid)
            {
                entry.Ghost.Destroy();
            }
        }
    }

    /// <summary>
    /// The oldest outstanding request, claimed by the authoritative spawn that just
    /// attached for it - destroying that request's predicted echo in the same
    /// motion, so nobody ever sees both at once. False, with <paramref name="payload"/>
    /// left at its default, when nothing is waiting: a spawn arrived with no
    /// request queued for it, which should not happen on the normal path but is
    /// the caller's call to handle, not this queue's.
    /// </summary>
    public bool TryReconcile(out T payload)
    {
        if (_pending.Count == 0)
        {
            payload = default!;
            return false;
        }
        Pending entry = _pending.Dequeue();
        if (entry.Ghost.IsValid)
        {
            entry.Ghost.Destroy();
        }
        payload = entry.Payload;
        return true;
    }

    /// <summary>
    /// Discards every outstanding request, destroying each one's predicted echo.
    /// Call this when the owner itself stops being able to receive replies for
    /// them - Whisper calls it when its player dies, so a shot pending at the
    /// moment of death does not sit waiting for a projectile that will never be
    /// credited to anyone.
    /// </summary>
    public void Clear()
    {
        while (_pending.Count > 0)
        {
            Pending entry = _pending.Dequeue();
            if (entry.Ghost.IsValid)
            {
                entry.Ghost.Destroy();
            }
        }
    }
}
