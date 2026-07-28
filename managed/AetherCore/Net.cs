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
/// State replication is automatic once a session is live, and it is
/// CLIENT-AUTHORITATIVE: the peer that owns an entity simulates it and sends its
/// changed <c>[Replicated]</c> script properties and replicated component fields
/// each tick, and the host relays them to everyone else. Nothing here has to be
/// pumped. Your own character therefore responds to your own input with no round
/// trip and no correction of any kind - what you simulate IS what the others see.
/// </para>
/// <para>
/// Transform smoothing is an opt-in on top of that, and it applies to OTHER
/// people's entities only: one with a <c>NetworkTransform</c> component alongside
/// its <c>NetworkIdentity</c> is rendered slightly in the past so motion between
/// snapshots is smooth, while one without has each received snapshot applied
/// straight to its transform, so it visibly snaps. Neither ever touches an entity
/// this peer owns.
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

    /// <summary>Start listening on <paramref name="port"/> as the session host.
    /// Scene-placed networked entities are given their network ids immediately.
    /// Returns false if the port is unavailable - see <see cref="LastError"/>.</summary>
    /// <param name="port">UDP port to listen on.</param>
    /// <param name="maxConnections">
    /// How many simultaneous CLIENT connections to accept. The host is not one of them,
    /// so a four-player game hosts with three. A joiner past the cap is refused with a
    /// reason it can read from <see cref="DisconnectReason"/> and show the player, not
    /// dropped silently - which is the only version of a player cap a game can present
    /// honestly.
    /// </param>
    public static bool Host(int port, int maxConnections = 32)
        => Native.aether_net_host((ushort)port, maxConnections) != 0;

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

    /// <summary>
    /// Why the last link ended, when the far end ended it DELIBERATELY and said why -
    /// a refusal ("Server is full"), or a host closing the session. Empty for every
    /// other kind of ending.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The empty case is as informative as the non-empty one, and the pair is what a
    /// reconnect policy is built on: a link that dropped with no reason was an
    /// ACCIDENT - a timeout, a pulled cable, a crashed host - and is worth retrying,
    /// while one that ended with a reason was a decision and retrying it would only
    /// get the same answer. A game that retries both looks broken to the player it
    /// just threw out.
    /// </para>
    /// <para>
    /// Survives the session it describes, deliberately: the session is already gone by
    /// the time a script notices. It is cleared when the next <see cref="Host"/> or
    /// <see cref="Connect"/> begins, so the previous session's reason cannot be
    /// mistaken for this one's.
    /// </para>
    /// </remarks>
    public static unsafe string DisconnectReason
    {
        get
        {
            Span<byte> buffer = stackalloc byte[256];
            fixed (byte* ptr = buffer)
            {
                int written = Native.aether_net_disconnect_reason(ptr, buffer.Length);
                return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
            }
        }
    }

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

    /// <summary>
    /// Whether this process decides <paramref name="entity"/>'s state this frame:
    /// true for what this peer OWNS, and true offline. Guard simulation with this
    /// rather than with <see cref="IsHost"/>.
    /// </summary>
    /// <remarks>
    /// The framework is client-authoritative: the owner of an entity simulates it and
    /// replicates the result, and the host relays. So this gives the same answer as
    /// <see cref="IsOwner"/> - not because one is redundant, but because they are two
    /// different questions ("may I move this" and "is this mine") that this model
    /// answers the same way. Note the host is NOT authoritative for a client's
    /// character any more; it was under the previous model.
    /// </remarks>
    public static bool HasAuthority(Entity entity) => Native.aether_net_has_authority(entity.Id) != 0;

    /// <summary>Whether this peer's connection owns <paramref name="entity"/> - the
    /// test for "is this my player". True offline.</summary>
    public static bool IsOwner(Entity entity) => Native.aether_net_is_owner(entity.Id) != 0;

    // ── Players ─────────────────────────────────────────────────────────────────

    /// <summary>
    /// Set a player entity's display name, adding the Net Player component if it has
    /// none. Independent of any session, so a name chosen on the menu survives into the
    /// session that later replicates it.
    /// </summary>
    /// <remarks>
    /// REFUSED, with a warning and no write, on an entity this peer does not own. A
    /// display name is replicated state and the owner of an entity is authoritative for
    /// its state, so a name written here by anybody else is overwritten by the owner's
    /// next send - whether it survives at all is a race, and the visible symptom is
    /// "some players' names do not show". The owner sets its own name; replication
    /// carries it everywhere else. Offline, and for the host's own player, this peer IS
    /// the owner and the call behaves exactly as it reads.
    /// </remarks>
    public static void SetPlayerName(Entity entity, string name)
        => Native.aether_net_set_player_name(entity.Id, name);

    /// <summary>
    /// Adopt <paramref name="desired"/> as this player's display name, stepping around
    /// a name an earlier player already has: "Alice" becomes "Alice (2)" when somebody
    /// ahead of us is already Alice. Returns the name actually adopted, or an empty
    /// string if the call was refused.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Only the OWNER of <paramref name="entity"/> may claim its name - see
    /// <see cref="SetPlayerName"/> for why - and offline that is always this peer, so
    /// single-player code needs no role test.
    /// </para>
    /// <para>
    /// Safe, and intended, to call every frame. Players are ordered by connection (the
    /// host first), a player only ever steps around players AHEAD of it, and the
    /// resolution runs from the desired name rather than from the current one: two
    /// peers picking the same name in the same frame therefore settle - the later one
    /// moves - instead of both moving and colliding again. There is no round trip and
    /// no peer writing another peer's field.
    /// </para>
    /// <para>
    /// A blank or unprintable name becomes "Player"; names are ASCII-only, matching the
    /// font pipeline.
    /// </para>
    /// </remarks>
    public static unsafe string ClaimPlayerName(Entity entity, string desired)
    {
        Span<byte> buffer = stackalloc byte[256];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_net_claim_player_name(entity.Id, desired, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

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
