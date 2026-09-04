using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>
/// Progress of a <see cref="Net.HostWithCode"/> or <see cref="Net.JoinByCode"/>
/// attempt. Mirrors the engine's <c>NetworkContext::TraversalState</c> exactly -
/// the value comes straight off that wire-adjacent enum, so the member order here
/// must never change.
/// </summary>
public enum NetTraversalState
{
    /// <summary>No traversal attempt is running.</summary>
    Idle,

    /// <summary>Asking the router for a port via UPnP/NAT-PMP/PCP.</summary>
    Mapping,

    /// <summary>Publishing and fetching candidates over the chosen signaling backend.</summary>
    Signaling,

    /// <summary>Exchanging connectivity checks against the peer's candidates.</summary>
    Punching,

    /// <summary>
    /// A punch failed outright (typically a symmetric NAT on one side) and traffic is
    /// being carried through a TURN relay instead, if one is configured and allowed -
    /// see <see cref="Net.ConfigureRelay"/>. Skipped straight to <see cref="Failed"/>
    /// when no relay is available to try.
    /// </summary>
    // ORDINAL POSITION IS LOAD-BEARING: this value is compared by ordinal against the
    // engine's own TraversalState enum. It must stay exactly here - between Punching
    // and Connecting - or every peer starts reporting the wrong state. Never reorder.
    Relaying,

    /// <summary>A path opened; establishing the session connection over it.</summary>
    Connecting,

    /// <summary>Connected and carrying session traffic.</summary>
    Connected,

    /// <summary>The attempt gave up - see <see cref="Net.TraversalError"/>.</summary>
    Failed,
}

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
    /// Whether this peer is standing in the scene the session's replicated entities
    /// belong to. True by default, so a game that never touches it behaves exactly as it
    /// always has.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Set it FALSE before starting a join that is going to be waited out somewhere else
    /// - a title screen showing "Connecting..." - and TRUE once the gameplay scene is
    /// loaded. <see cref="NetSessionDirector"/> already sets it true when it attaches, so
    /// a game built on that only has to say when it is NOT ready.
    /// </para>
    /// <para>
    /// <b>Why it exists.</b> A replicated entity is created into whatever scene this peer
    /// happens to be in. The host answers a join with a welcome and a spawn for every
    /// entity already in the session, in one burst, so a client still sitting on its menu
    /// builds every other player into the MENU and destroys them a frame later on the
    /// scene change - permanently, because the host remembers per connection what it has
    /// already sent. The client arrives in an empty level while the host believes it
    /// spawned everybody, and nothing is logged on either side.
    /// </para>
    /// <para>
    /// While it is false this peer ignores every inbound spawn and every inbound state
    /// packet - none of it describes anything it is holding - and NOTHING IS QUEUED.
    /// Setting it true again discards whatever the session left in the world being left
    /// behind and asks the host for the world from scratch, so a player who gives up and
    /// goes back to the menu leaves nothing behind to be applied later.
    /// </para>
    /// <para>
    /// It is a statement about how this game joins rather than session state, so
    /// <see cref="Disconnect"/> does not reset it - and <see cref="Connect"/> ends any
    /// previous session internally, so a flag cleared by that would be cleared out from
    /// under the menu that set it a line earlier.
    /// </para>
    /// </remarks>
    public static bool ReplicationReady
    {
        get => Native.aether_net_is_replication_ready() != 0;
        set => Native.aether_net_set_replication_ready(value ? 1 : 0);
    }

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

    // ── NAT traversal ───────────────────────────────────────────────────────────

    /// <summary>
    /// Generates a fresh room code to show the hosting player. Independent of any
    /// session - call it before <see cref="HostWithCode"/> to have something to
    /// display, and again if the player asks for a different one.
    /// </summary>
    public static unsafe string NewRoomCode()
    {
        Span<byte> buffer = stackalloc byte[16];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_net_new_room_code(ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>
    /// Signal over the local network: candidates go out as LAN broadcasts, so only a
    /// peer on the same network segment ever sees them and no server is needed at
    /// all. This is the backend in effect until one of these two is called, so a
    /// same-network game needs neither.
    /// </summary>
    public static void UseLanSignaling() => Native.aether_net_configure_signaling(0, null);

    /// <summary>
    /// Signal through a rendezvous server at <paramref name="address"/>
    /// ("host:port"): both peers post their candidates there and it relays them to
    /// each other, which is what lets two players behind two different routers find
    /// one another. Call before <see cref="HostWithCode"/> or
    /// <see cref="JoinByCode"/> - it only chooses where the next attempt looks.
    /// </summary>
    public static void UseRendezvousSignaling(string address)
        => Native.aether_net_configure_signaling(1, address);

    /// <summary>
    /// Host a session with no port forwarding required: opens a port with UPnP/
    /// NAT-PMP/PCP if the router offers one, publishes this peer's candidates under
    /// <paramref name="code"/> through whichever signaling backend was chosen, and
    /// punches a hole to the first peer that answers. This call only starts that -
    /// poll <see cref="TraversalState"/> for progress and <see cref="IsConnected"/>
    /// for the outcome, the same as <see cref="Host"/>.
    /// </summary>
    /// <param name="port">
    /// Local UDP port to bind. 0 (the default) lets the OS pick one; the endpoint a
    /// joiner actually reaches is whatever the router maps or the punch discovers,
    /// never this number, so there is normally no reason to pin it.
    /// </param>
    /// <param name="maxConnections">Same meaning as on <see cref="Host"/>.</param>
    /// <returns>False if the local bind failed outright. A true return means
    /// traversal has started, not that a peer has connected.</returns>
    public static bool HostWithCode(string code, int port = 0, int maxConnections = 32)
        => Native.aether_net_host_with_code(code, (ushort)port, maxConnections) != 0;

    /// <summary>
    /// Join the session published under <paramref name="code"/>: fetches the host's
    /// candidates through whichever signaling backend was chosen, punches a hole to
    /// them, and connects through the resulting path once it opens. Poll
    /// <see cref="TraversalState"/> for progress and <see cref="IsConnected"/> for
    /// the outcome.
    /// </summary>
    /// <returns>False if the local bind failed or <paramref name="code"/> is not a
    /// well-formed room code. A true return means traversal has started, not that
    /// it will succeed.</returns>
    public static bool JoinByCode(string code) => Native.aether_net_join_by_code(code) != 0;

    /// <summary>
    /// Where a <see cref="HostWithCode"/> or <see cref="JoinByCode"/> attempt
    /// currently stands. <see cref="NetTraversalState.Idle"/> both before either has
    /// been called and in a build with no session at all, so a title screen can read
    /// this before anything has connected.
    /// </summary>
    public static NetTraversalState TraversalState
        => (NetTraversalState)Native.aether_net_traversal_state();

    /// <summary>
    /// Why the last traversal attempt reached <see cref="NetTraversalState.Failed"/>
    /// - a STUN timeout, a mapping refusal, a punch that never opened. Empty while
    /// idle, in progress, or connected, the same empty-means-nothing-to-report
    /// convention as <see cref="DisconnectReason"/>.
    /// </summary>
    public static unsafe string TraversalError
    {
        get
        {
            Span<byte> buffer = stackalloc byte[256];
            fixed (byte* ptr = buffer)
            {
                int written = Native.aether_net_traversal_error(ptr, buffer.Length);
                return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
            }
        }
    }

    /// <summary>
    /// Points the connect ladder at a TURN relay server, the LAST resort for a NAT
    /// that a hole punch cannot get through - a symmetric NAT hands out a different
    /// public mapping per destination, which breaks the assumption punching depends
    /// on (see docs/multiplayer-relay.md). Call this from a settings/title screen
    /// BEFORE <see cref="HostWithCode"/> or <see cref="JoinByCode"/>; the ladder only
    /// reaches for it after a punch has already failed, never in parallel with one.
    /// </summary>
    /// <param name="host">TURN server hostname or IP. Empty clears the relay.</param>
    /// <param name="port">TURN server port (3478 for the reference coturn config).</param>
    /// <param name="username">Long-term-credential username the server expects.</param>
    /// <param name="password">Long-term-credential password the server expects.</param>
    /// <param name="allow">
    /// Whether the ladder may actually fall back to this relay. Defaults true, but
    /// every byte of a relayed match flows through this server twice (once each way)
    /// at whoever runs it's expense - show it to the player as a fallback they opt
    /// into, not a default you flip on for them.
    /// </param>
    /// <remarks>
    /// Writes straight through the same <c>network.*</c> settings
    /// <c>settings.toml</c>/<c>EngineSettings.toml</c> configure (see
    /// docs/multiplayer-relay.md) - there is no separate in-memory copy, so a value
    /// set here is exactly what the ladder reads and what a later <c>Save()</c> from
    /// the settings UI would persist. Do NOT call this with a credential compiled
    /// into a shipped client: every copy of the game then carries the same shared
    /// secret, readable by any player who inspects the binary. Mint one server-side
    /// per session instead, and call this with the short-lived result - this engine
    /// does not do that minting for you yet.
    /// </remarks>
    public static void ConfigureRelay(string host, int port, string username, string password, bool allow = true)
        => Native.aether_net_configure_relay(host, (ushort)port, username, password, allow ? 1 : 0);

    /// <summary>
    /// True once a relay is both configured (a non-empty host from
    /// <see cref="ConfigureRelay"/>) and allowed. When false, a symmetric-NAT peer
    /// that fails to punch goes straight to <see cref="NetTraversalState.Failed"/>
    /// with no relay rung to fall back to - so a game offering multiplayer across
    /// arbitrary networks should show relay setup as a fallback path, not assume it.
    /// </summary>
    public static bool RelayConfigured => Native.aether_net_relay_configured() != 0;

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

    /// <summary>
    /// How long a round trip to the host costs this player, in milliseconds. 0 for the
    /// host's own player, and 0 offline - both of which are the honest reading rather
    /// than a missing value.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Readable on EVERY peer for EVERY player. Only the machine at one end of a link
    /// can measure it, so each peer stamps its own player and replication carries the
    /// number outward with that player's other owned state - which is why this answers
    /// for other people's players as well as your own, and why nothing has to be pumped.
    /// </para>
    /// <para>
    /// The measurement is the transport's own: ENet acknowledges reliable traffic and
    /// keeps a smoothed estimate from it, so this costs no packets and cannot disagree
    /// with what the link is actually doing. It settles over the first second or so of a
    /// session, and reads 0 until the first acknowledgement lands.
    /// </para>
    /// </remarks>
    public static uint GetPlayerPing(Entity entity) => Native.aether_net_get_player_ping(entity.Id);

    /// <summary>This peer's own round-trip time to the host in milliseconds; 0 on the
    /// host and offline. <see cref="GetPlayerPing"/> is usually what a HUD wants - this
    /// is the raw reading, before it has been stamped onto anything.</summary>
    public static uint RoundTripMs => Native.aether_net_round_trip_ms();

    /// <summary>
    /// The connection that owns <paramref name="entity"/>. Reports this peer's own
    /// connection for an entity that is not replicated or has no owner yet, which is
    /// every entity in an offline game.
    /// </summary>
    /// <remarks>
    /// Connection ids are stable for a connection's lifetime and never reused within a
    /// session, so this is the right key for per-player bookkeeping. 0 is the host.
    /// </remarks>
    public static uint OwnerOf(Entity entity) => Native.aether_net_owner_of(entity.Id);

    /// <summary>
    /// Every player entity this peer is currently holding - the ones with a Net Player
    /// component, whoever owns them.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Answered from the WORLD rather than from the session, which is what makes it work
    /// on a client: a client is told nothing about connections (see
    /// <see cref="Connections"/>), but it is holding every replicated player it can see.
    /// This is therefore the one roster query with the same meaning on every peer.
    /// </para>
    /// <para>
    /// The order is the world's, not the join order, and it is not stable - sort by
    /// <see cref="OwnerOf"/> if the list is going on screen.
    /// </para>
    /// </remarks>
    public static unsafe Entity[] Players
    {
        get
        {
            int count = Native.aether_net_players(null, 0);
            if (count <= 0)
            {
                return [];
            }
            Entity[] players = new Entity[count];
            fixed (Entity* ptr = players)
            {
                // Entity is a single uint field, so its storage is layout-compatible
                // with a uint buffer - the same assumption Tags.GetEntitiesWith makes.
                int written = Native.aether_net_players((uint*)ptr, count);
                return written == count ? players : players[..System.Math.Max(written, 0)];
            }
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
