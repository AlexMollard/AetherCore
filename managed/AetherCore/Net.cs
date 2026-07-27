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
    /// to <paramref name="entity"/>, resolved by <paramref name="methodName"/>.
    /// </summary>
    /// <remarks>
    /// On a client a <see cref="NetRpcTarget.Server"/> call is sent to the host and
    /// invoked there. On the host, and in an unnetworked game, the host is this
    /// process, so the same call runs locally.
    /// </remarks>
    /// <exception cref="ArgumentException">
    /// More than one argument was passed, or a single argument was passed that is
    /// not a <see cref="string"/> - see the marshalling note on <see cref="Net"/>.
    /// </exception>
    public static unsafe void CallServer(Entity entity, string methodName, params object[] args)
    {
        if (args.Length > 1 || (args.Length == 1 && args[0] is not string))
        {
            throw new ArgumentException(
                "Net.CallServer supports at most one string argument today.", nameof(args));
        }

        if (args.Length == 0)
        {
            Native.aether_net_call_server(entity.Id, methodName, null, 0);
            return;
        }

        string arg = (string)args[0];
        byte[] blob = Encoding.UTF8.GetBytes(arg);
        fixed (byte* ptr = blob)
        {
            Native.aether_net_call_server(entity.Id, methodName, ptr, blob.Length);
        }
    }
}
