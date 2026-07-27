using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>
/// Multiplayer: hosting and joining a session, spawning replicated entities, and
/// asking who owns what.
/// </summary>
/// <remarks>
/// <para>
/// EVERY member here is safe to call with no session. A title screen reads
/// <see cref="IsHost"/> before anything has connected, and a single-player build
/// never starts one at all; those calls return <c>false</c>/<c>0</c>/empty rather
/// than throwing. Offline, <see cref="HasAuthority"/> and <see cref="IsOwner"/>
/// both report <c>true</c> - local state is the only state - so code written for
/// multiplayer runs unchanged in single-player.
/// </para>
/// <para>
/// State replication is automatic once a session is live: the host sends changed
/// <c>[Replicated]</c> script properties and replicated component fields to every
/// client each tick, and clients apply them. Nothing here has to be pumped.
/// </para>
/// <para>
/// Transform smoothing is an opt-in on top of that: an entity needs a
/// <c>NetworkTransform</c> component, alongside its <c>NetworkIdentity</c>, before a
/// client eases the owned entity's position or interpolates a remote one. An entity
/// with a <c>NetworkIdentity</c> but no <c>NetworkTransform</c> still replicates, but
/// each received snapshot is applied straight to its transform with no smoothing, so
/// it visibly snaps.
/// </para>
/// <para>
/// RPC argument marshalling is deliberately minimal: a call carries either no
/// arguments or a single <see cref="string"/> argument. Passing anything else
/// throws <see cref="ArgumentException"/> here, in the calling script's own code,
/// rather than failing silently or crossing the wire malformed.
/// </para>
/// </remarks>
public static class Net
{
    // ── Session ─────────────────────────────────────────────────────────────────

    /// <summary>Start listening on <paramref name="port"/> as the authoritative host.
    /// Scene-placed networked entities are given their network ids immediately.
    /// Returns false if the port is unavailable - see <see cref="LastError"/>.</summary>
    public static bool Host(int port, int maxPeers = 32)
        => Native.aether_net_host((ushort)port, maxPeers) != 0;

    /// <summary>Begin connecting to a host. Returns false only if the address could
    /// not be resolved or a socket could not be opened; a successful return means the
    /// attempt started, not that it succeeded - poll <see cref="IsConnected"/>.</summary>
    public static bool Connect(string ip, int port)
        => Native.aether_net_connect(ip, (ushort)port) != 0;

    /// <summary>Leave the session (or stop hosting). Safe when not connected.</summary>
    public static void Disconnect() => Native.aether_net_disconnect();

    /// <summary>True when this process is the authoritative host. False offline.</summary>
    public static bool IsHost => Native.aether_net_is_host() != 0;

    /// <summary>True when this process is a client of a remote host. False offline.</summary>
    public static bool IsClient => Native.aether_net_is_client() != 0;

    /// <summary>True once a session can carry traffic: a host is connected as soon as
    /// it is listening, a client only once the host has assigned it a connection id.</summary>
    public static bool IsConnected => Native.aether_net_is_connected() != 0;

    /// <summary>This peer's connection id. 0 on the host and while offline, which is
    /// also the id the host owns entities under.</summary>
    public static uint LocalConnectionId => Native.aether_net_local_connection_id();

    /// <summary>
    /// Host only: the connection ids currently joined, newest last. Empty on a client
    /// and offline - a client is told nothing about its peers, and an unnetworked
    /// build has none.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This is the only way a project learns that somebody joined or left: there is no
    /// join/leave callback, so the host polls this and diffs it against what it saw
    /// last frame. The ids themselves are stable for a connection's lifetime and never
    /// reused within a session, so they are safe to key per-player state by.
    /// </para>
    /// <para>
    /// A leaver's entities are already gone by the time it disappears from here - the
    /// framework despawns everything a dropped connection owned before the frame's
    /// scripts run - so a project reacting to a departure is freeing its OWN
    /// bookkeeping, not the entity.
    /// </para>
    /// </remarks>
    public static unsafe uint[] Connections
    {
        get
        {
            int count = Native.aether_net_connections(null, 0);
            if (count <= 0)
            {
                return [];
            }
            uint[] ids = new uint[count];
            fixed (uint* ptr = ids)
            {
                int written = Native.aether_net_connections(ptr, count);
                // A connection can drop between the two calls, so trust the second
                // count rather than the array we sized from the first.
                return written == count ? ids : ids[..System.Math.Max(written, 0)];
            }
        }
    }

    /// <summary>How many connections have joined, without allocating the id array.
    /// 0 on a client and offline - see <see cref="Connections"/>.</summary>
    public static unsafe int ConnectionCount => Native.aether_net_connections(null, 0);

    /// <summary>The last transport error, or an empty string if there was none.</summary>
    public static unsafe string LastError
    {
        get
        {
            Span<byte> buffer = stackalloc byte[256];
            fixed (byte* ptr = buffer)
            {
                int written = Native.aether_net_last_error(ptr, buffer.Length);
                return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
            }
        }
    }

    // ── Replicated entities ─────────────────────────────────────────────────────

    /// <summary>Host only. Instantiate <paramref name="prefab"/> and replicate it to
    /// every client, owned by <paramref name="owner"/> (0 = the host owns it).
    /// Returns an invalid entity when not hosting or the prefab does not exist.</summary>
    public static Entity Spawn(string prefab, Vector3 position, uint owner = 0)
        => new(Native.aether_net_spawn(prefab, position, owner));

    /// <summary>Host only over the wire, but always destroys the entity locally, so
    /// the same call works in a single-player build.</summary>
    public static void Despawn(Entity entity) => Native.aether_net_despawn(entity.Id);

    /// <summary>Whether this process decides <paramref name="entity"/>'s state: true on
    /// the host for everything, true on a client only for what it owns, true offline.
    /// Guard state-changing logic with this rather than with <see cref="IsHost"/>.</summary>
    public static bool HasAuthority(Entity entity) => Native.aether_net_has_authority(entity.Id) != 0;

    /// <summary>Whether this peer's connection owns <paramref name="entity"/> - the
    /// test for "is this my player". True offline.</summary>
    public static bool IsOwner(Entity entity) => Native.aether_net_is_owner(entity.Id) != 0;

    /// <summary>
    /// Writes <paramref name="correctionRate"/> and <paramref name="snapDistance"/>
    /// into <paramref name="entity"/>'s <c>NetworkTransform</c> component (see the
    /// remarks on <see cref="Net"/> for what that component does). A no-op if the
    /// entity has no <c>NetworkTransform</c> - there is nothing to tune.
    /// </summary>
    /// <param name="correctionRate">How fast the locally-owned entity's predicted
    /// position eases toward the host's authoritative one.</param>
    /// <param name="snapDistance">Position error beyond which correction snaps
    /// instead of easing.</param>
    public static void SetTransformTuning(Entity entity, float correctionRate, float snapDistance)
        => Native.aether_net_set_transform_tuning(entity.Id, correctionRate, snapDistance);

    // ── Players ─────────────────────────────────────────────────────────────────

    /// <summary>Set a player entity's display name, adding the Net Player component if
    /// it has none. Independent of any session, so a name chosen on the menu survives
    /// into the session that later replicates it.</summary>
    public static void SetPlayerName(Entity entity, string name)
        => Native.aether_net_set_player_name(entity.Id, name);

    /// <summary>A player entity's display name, or an empty string if it has no Net
    /// Player component.</summary>
    public static unsafe string GetPlayerName(Entity entity)
    {
        Span<byte> buffer = stackalloc byte[256];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_net_get_player_name(entity.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    // ── RPCs ────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Invokes a <see cref="NetRpcAttribute"/> method declared on a script attached
    /// to <paramref name="entity"/>, resolved by <paramref name="methodName"/>, and
    /// dispatches it to wherever the method's <see cref="NetRpcTarget"/> says it
    /// runs. This is the one send path; the direction is read off the declaration, so
    /// a call site cannot contradict it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Returns false, having sent nothing, whenever the call cannot be made: no
    /// attached script declares that RPC; a client tried to originate a
    /// <see cref="NetRpcTarget.Client"/> or <see cref="NetRpcTarget.Multicast"/> call
    /// (only the host may); or the call has to cross the wire and
    /// <paramref name="entity"/> is not replicated, so it has no name to be addressed
    /// by on the far end. A refused call is never quietly downgraded to a local one -
    /// that would run against unreplicated state and report success.
    /// </para>
    /// <para>
    /// With no session every target runs locally, so single-player code needs no role
    /// test.
    /// </para>
    /// </remarks>
    /// <returns>
    /// True if the call was routed - encoded and sent to at least one recipient, run
    /// locally, or both. This reports that the call was ALLOWED and addressed, never
    /// that it arrived: nothing is acknowledged, and one routed case reaches nobody at
    /// all. On the host a <see cref="NetRpcTarget.Client"/> call is addressed to the
    /// connection owning the entity, and if that connection has already dropped the
    /// call is still allowed, has no recipient left, does not run locally, and still
    /// returns true. That is deliberate - "the owner left" is not a caller error, and
    /// reporting it as a refusal would be indistinguishable from the genuine refusals
    /// listed above, which a caller may well want to treat differently.
    /// </returns>
    /// <exception cref="ArgumentException">
    /// More than one argument was passed, or a single argument was passed that is
    /// not a <see cref="string"/> - see the marshalling note on <see cref="Net"/>.
    /// </exception>
    public static bool Call(Entity entity, string methodName, params object[] args)
        => Dispatch(entity, methodName, args, expectedTarget: -1, nameof(Call));

    /// <summary>
    /// <see cref="Call"/> restricted to <see cref="NetRpcTarget.Server"/> methods: on
    /// a client the call is sent to the host and invoked there, and on the host (or
    /// in an unnetworked game) it runs locally.
    /// </summary>
    /// <remarks>
    /// The explicit spelling, kept because a server RPC reads better at the call site
    /// when the direction is the point. A method whose attribute declares a different
    /// target is refused rather than re-routed.
    /// </remarks>
    /// <exception cref="ArgumentException">
    /// More than one argument was passed, or a single argument was passed that is
    /// not a <see cref="string"/> - see the marshalling note on <see cref="Net"/>.
    /// </exception>
    public static void CallServer(Entity entity, string methodName, params object[] args)
        => Dispatch(entity, methodName, args, expectedTarget: (int)NetRpcTarget.Server, nameof(CallServer));

    // ── Input ───────────────────────────────────────────────────────────────────

    /// <summary>
    /// Submits one frame of input for an entity this peer OWNS, to be applied
    /// wherever the simulation is authoritative. The named method - an ordinary
    /// <c>[NetRpc(NetRpcTarget.Server)]</c> handler on a script attached to
    /// <paramref name="entity"/> - runs locally straight away, and on a client the
    /// payload is also sent to the host so it runs there too.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This is the client-to-host half of a host-authoritative session, and the
    /// reason a client's character moves on everybody else's screen: state
    /// replication only ever flows host-to-client, so without this the host has no
    /// input for a client's player and simulates it as an unattended body.
    /// </para>
    /// <para>
    /// The local invoke is not a convenience - it IS the client-side prediction. The
    /// owner acts on its own input immediately, the host applies the same payload
    /// through the same handler a round trip later, and the framework eases the
    /// owner's predicted position toward the host's answer (see the
    /// <c>NetworkTransform</c> remarks on <see cref="Net"/>). Both peers therefore
    /// run one implementation of the movement, not two.
    /// </para>
    /// <para>
    /// WHAT IS IN THE PAYLOAD IS ENTIRELY YOURS. The framework moves the bytes and
    /// never looks inside: it has no idea what "jump" means. Call this every frame
    /// with the current input state - transmission is paced and deduplicated for you,
    /// so an unchanged payload does not flood the wire, while a payload that changed
    /// (the one frame a button went down) is sent at once.
    /// </para>
    /// <para>
    /// Delivery is unreliable and sequenced on its own channel: input that arrives
    /// late is worthless, and the host applies only strictly-newer submissions, so a
    /// reordered packet is dropped rather than rewinding the character.
    /// </para>
    /// </remarks>
    /// <returns>
    /// False, having done nothing, when no attached script declares that handler,
    /// when the handler declares a direction other than
    /// <see cref="NetRpcTarget.Server"/>, or when a client submits input for an
    /// entity it does not own. True when the input was applied locally, sent, or
    /// both - as with <see cref="Call"/>, never that it arrived.
    /// </returns>
    /// <exception cref="ArgumentException">
    /// More than one argument was passed, or a single argument was passed that is
    /// not a <see cref="string"/> - see the marshalling note on <see cref="Net"/>.
    /// </exception>
    public static unsafe bool SendInput(Entity entity, string methodName, params object[] args)
    {
        if (args.Length > 1 || (args.Length == 1 && args[0] is not string))
        {
            throw new ArgumentException(
                $"Net.{nameof(SendInput)} supports at most one string argument today.", nameof(args));
        }

        if (args.Length == 0)
        {
            return Native.aether_net_send_input(entity.Id, methodName, null, 0) != 0;
        }

        byte[] blob = Encoding.UTF8.GetBytes((string)args[0]);
        fixed (byte* ptr = blob)
        {
            return Native.aether_net_send_input(entity.Id, methodName, ptr, blob.Length) != 0;
        }
    }

    // The one marshalling site. `expectedTarget` is -1 for "whatever the method
    // declares" and a NetRpcTarget value for the explicit spellings, which the native
    // half compares against the declaration and refuses on a mismatch.
    private static unsafe bool Dispatch(Entity entity, string methodName, object[] args, int expectedTarget,
        string caller)
    {
        if (args.Length > 1 || (args.Length == 1 && args[0] is not string))
        {
            throw new ArgumentException(
                $"Net.{caller} supports at most one string argument today.", nameof(args));
        }

        if (args.Length == 0)
        {
            return Native.aether_net_call_rpc(entity.Id, methodName, null, 0, expectedTarget) != 0;
        }

        string arg = (string)args[0];
        byte[] blob = Encoding.UTF8.GetBytes(arg);
        fixed (byte* ptr = blob)
        {
            return Native.aether_net_call_rpc(entity.Id, methodName, ptr, blob.Length, expectedTarget) != 0;
        }
    }
}
