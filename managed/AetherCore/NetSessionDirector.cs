using System.Collections.Generic;
using System.Numerics;

namespace AetherCore;

/// <summary>One player in a live session: which connection they arrived on, the entity
/// spawned for them, and the display name they were introduced under.</summary>
/// <remarks>
/// A snapshot, not a live view. <see cref="Entity"/> is already destroyed by the time a
/// departure is reported - the framework despawns everything a dropped connection owned
/// before the frame's scripts run - which is exactly why <see cref="Name"/> is carried
/// here rather than read back off the entity when it is needed.
/// </remarks>
public readonly struct NetSessionPlayer
{
    /// <summary>Create a roster entry.</summary>
    public NetSessionPlayer(uint connection, Entity entity, string name)
    {
        Connection = connection;
        Entity = entity;
        Name = name;
    }

    /// <summary>The connection this player arrived on. Stable for the connection's
    /// lifetime and never reused within a session. 0 is the HOST.</summary>
    public uint Connection { get; }

    /// <summary>The entity spawned for this player. Invalid once they have left.</summary>
    public Entity Entity { get; }

    /// <summary>The display name they were introduced under.</summary>
    public string Name { get; }

    /// <summary>Round-trip time to the host in milliseconds, read live off the entity.
    /// 0 for the host's own player, offline, and for a departed one.</summary>
    /// <remarks>A property rather than a captured field because it changes every tick and
    /// a roster entry does not: a snapshotted ping would be a stale number on screen
    /// pretending to be a live one.</remarks>
    public uint PingMs => Entity.IsValid ? Net.GetPlayerPing(Entity) : 0u;

    /// <summary>Whether this is the session host - the peer everybody else's latency is
    /// measured against, and the one with none of its own.</summary>
    public bool IsHost => Connection == 0u;
}

/// <summary>
/// Runs a multiplayer session for one gameplay scene: spawns a player per connection,
/// keeps the roster, decides what a link ENDING means, and puts the player back on the
/// menu when it is over.
/// </summary>
/// <remarks>
/// <para>
/// Attach one subclass of this to a scene entity in the gameplay scene. A subclass with
/// an empty body is a complete, working session; the overridable members below are for
/// games that want to say something about arrivals and departures, or that use a key
/// other than Escape to leave.
/// </para>
/// <para>
/// On the <b>host</b> this owns the player population: one <see cref="PlayerPrefab"/>
/// for the host itself the moment the scene starts, one more for every connection that
/// joins, each at its own spawn marker. A leaver's entities are despawned by the
/// framework before this next ticks, so the only thing left to do is hand its spawn
/// point back and say who went.
/// </para>
/// <para>
/// On a <b>client</b> this does almost nothing while things are going well: the players
/// arrive by replication, and this peer's own name is written by whatever script owns
/// the local player, because the owner of an entity is authoritative for it. What is
/// left is deciding what a link ENDING means.
/// </para>
/// <para>
/// <b>Not every ending is the same, and treating them alike is what makes a session
/// feel broken.</b> A host that quit, and a host this client was refused by, both arrive
/// with a reason attached (<see cref="Net.DisconnectReason"/>): somebody decided, and
/// there is nothing to retry. A link that simply stopped - a timeout, a dropped packet
/// storm, a laptop lid - arrives with nothing to say, and that silence is the signal to
/// try coming back. So this reconnects a bounded number of times on the second kind and
/// never on the first, and a player who asked to leave is not dragged back into a
/// session they just left.
/// </para>
/// <para>
/// There is no join/leave callback in the framework, so the host polls
/// <see cref="Net.Connections"/> and diffs it. That is cheap (a handful of ids) and is
/// the only signal available. The matching client-side signal is
/// <see cref="Net.IsConnected"/> going false.
/// </para>
/// <para>
/// <b>There is no host migration.</b> When the host goes, the session goes: every client
/// tears its replicated entities down and returns to <see cref="ReturnScene"/>. That is a
/// deliberate non-goal, not an unfinished edge - promoting a client would mean
/// re-deriving the whole authoritative world on a peer that only ever saw snapshots
/// of it.
/// </para>
/// <para>
/// With no session at all - the scene opened straight from the editor - one player is
/// still spawned, so the level stays playable solo. <see cref="Net.IsOwner"/> is true
/// offline, so a player controller drives it exactly as it would online.
/// </para>
/// </remarks>
public abstract class NetSessionDirector : EntityScript
{
    /// <summary>What <see cref="NetSession.StatusMessage"/> is set to when the host goes
    /// away without a word and reconnecting has run out of attempts.</summary>
    public const string HostDisconnectedMessage = "Host disconnected";

    /// <summary>Sent back to the menu when the transport gave up on the FIRST connection
    /// before it was ever established.</summary>
    public const string UnreachableMessage = "Could not reach the host - nothing is listening there";

    /// <summary>Sent back to the menu when the first connection was neither accepted nor
    /// refused within <see cref="ConnectTimeoutSeconds"/>.</summary>
    public const string ConnectTimedOutMessage = "Timed out waiting for the host";

    /// <summary>
    /// How long a FIRST connection is given before this gives up on it and goes back to the
    /// menu.
    /// </summary>
    /// <remarks>
    /// Separate from <see cref="ReconnectTimeoutSeconds"/>, and longer, because the two are
    /// different questions. A reconnect is one of several bounded attempts at a host that
    /// answered a moment ago; a first connection is the only attempt there will be, against
    /// an address the player typed and may well have got wrong. Without this the level sits
    /// there indefinitely - the transport eventually drops the peer, but nothing was
    /// watching for that either, so a mistyped port produced an empty arena and no
    /// explanation.
    /// </remarks>
    public float ConnectTimeoutSeconds = 8.0f;

    /// <summary>How long "Connected." and "Reconnected." stay on screen. Long enough to
    /// read, short enough not to become part of the HUD.</summary>
    public float NoticeSeconds = 1.5f;

    /// <summary>Prefab spawned for each player. A bare stem, not a path - that is what
    /// <see cref="Net.Spawn"/> takes, and the name travels on the wire.</summary>
    public string PlayerPrefab = "player";

    /// <summary>Name stem of the scene's spawn markers: <c>Spawn0</c>, <c>Spawn1</c>, and
    /// so on. A player whose marker is missing spawns at the origin.</summary>
    public string SpawnPointPrefix = "Spawn";

    /// <summary>How many spawn markers the scene provides. Players beyond this many
    /// share, which is better than refusing to spawn them.</summary>
    public int SpawnPointCount = 4;

    /// <summary>World units each extra player sharing a marker is offset along X. Wide
    /// enough to clear a character, or two bodies spawn inside each other and the physics
    /// solver decides where they end up.</summary>
    public float SpawnSpread = 1.25f;

    /// <summary>Scene loaded when the session ends, i.e. the menu. Empty means "stay
    /// where we are", which is what a game with a single persistent scene wants.</summary>
    public string ReturnScene = string.Empty;

    /// <summary>How many times a silent drop is retried before giving up. Bounded, and
    /// small: three tries over roughly six seconds is long enough to ride out a hiccup
    /// and short enough that a player staring at a dead level is sent somewhere useful
    /// rather than left hoping.</summary>
    public int ReconnectAttempts = 3;

    /// <summary>Seconds between reconnect attempts.</summary>
    public float ReconnectDelaySeconds = 2.0f;

    /// <summary>Seconds a single reconnect attempt is given before it counts as failed.
    /// A connect that is never answered would otherwise hang the whole retry sequence on
    /// the first attempt.</summary>
    public float ReconnectTimeoutSeconds = 5.0f;

    // Which connection is standing on each spawn point, so two players never land on top
    // of each other. Unclaimed rather than 0, because 0 is the HOST's connection id - it
    // is a real occupant, not an empty slot.
    private const uint Unclaimed = uint.MaxValue;
    private uint[] _slotOwners = [];

    // Connections seen last frame, to diff Net.Connections against. Also the map from a
    // connection to the spawn point it was given, so a departure frees the right slot.
    private readonly Dictionary<uint, int> _slotByConnection = new();

    // Everyone currently in the session, rebuilt from the world every frame by
    // SyncRoster. Not accumulated, and deliberately not: see the remarks there.
    private readonly List<NetSessionPlayer> _players = new();

    // Host only: who the game has already been TOLD about, and under what name. A
    // departure is reported from this rather than from the roster, because by the time
    // anybody notices a player has gone their entity - and the name on it - is destroyed.
    private readonly Dictionary<uint, NetSessionPlayer> _introduced = new();

    // Arrivals and departures waiting for a game that can take them, oldest first. See
    // FlushNotices.
    private readonly List<(bool Joined, NetSessionPlayer Player)> _pendingNotices = new();

    private bool _localPlayerSpawned;
    private bool _wasInSession;

    // First-connection watchdog: how long this client has been waiting to be let in, and
    // how much longer a one-off notice ("Connected.") stays up.
    private float _connectElapsed;
    private float _noticeLeft;

    // Reconnect state. `_reconnecting` is the mode; the rest is one attempt's worth of
    // bookkeeping.
    private bool _reconnecting;
    private bool _attemptLive;
    private int _attemptsMade;
    private float _attemptElapsed;
    private float _untilNextAttempt;

    // On-screen line for reconnect progress. Created only when there is something to say
    // - an empty level with a dead link is exactly when the player has no other source of
    // information about what is happening.
    private Entity _status;

    /// <summary>
    /// This peer's own player: the one spawned for the host, or the solo player spawned
    /// with no session at all. Invalid on a client, whose player arrives by replication.
    /// </summary>
    /// <remarks>
    /// Host-owned and replicated, which is what makes it the right carrier for anything
    /// the host has to say to everybody - a multicast is refused unless it originates on
    /// the host, is addressed on the wire by the carrier's net id, and it has to outlive
    /// the players it talks about.
    /// </remarks>
    public Entity LocalPlayer { get; private set; }

    /// <summary>
    /// Everyone in the session, host first and then in join order. The same list on every
    /// peer, so a HUD built on it reads the same whether this process is hosting or not.
    /// </summary>
    /// <remarks>
    /// Rebuilt each frame from the players this peer is actually holding, rather than
    /// accumulated from arrivals. That is what lets a CLIENT have a roster at all: a
    /// client is told nothing about connections (see <see cref="Net.Connections"/>) but
    /// it is holding every replicated player it can see, so the world is the only source
    /// with the same answer on both roles. It also means there is exactly one roster
    /// rather than one per peer kind, and no way for it to drift out of step with what is
    /// on screen.
    /// </remarks>
    public IReadOnlyList<NetSessionPlayer> Players => _players;

    /// <summary>True while the bounded reconnect sequence is running.</summary>
    public bool IsReconnecting => _reconnecting;

    /// <inheritdoc/>
    /// <remarks>
    /// Attaching in the gameplay scene is what makes this peer ready to receive
    /// replicated entities: a spawn is applied into whatever scene the peer is standing
    /// in, so a client that joined from a menu must not be given the session's players
    /// until it is standing here. Declaring it in the SDK rather than leaving it to each
    /// project is deliberate - a game that forgets loses every player silently, which is
    /// exactly the failure this exists to remove. Setting it when it is already true (the
    /// default, and the case for a game that never waits on a menu) does nothing at all.
    /// </remarks>
    public override void OnAttach()
    {
        Net.ReplicationReady = true;

        _slotOwners = new uint[System.Math.Max(SpawnPointCount, 1)];
        for (int i = 0; i < _slotOwners.Length; i++)
        {
            _slotOwners[i] = Unclaimed;
        }

        // Nothing live and nothing to report: this scene was opened straight from the
        // editor. See NetSession.JoinRequested for why a stale static has to be cleared
        // here - it survives the editor's Play/Stop cycle, and a stale one would route a
        // genuine single-player session down the client branch.
        if (!Net.IsClient && !Net.IsHost && !Net.IsConnected && Net.DisconnectReason.Length == 0)
        {
            NetSession.JoinRequested = false;
        }
    }

    /// <inheritdoc/>
    public override void OnDetach() => HideStatus();

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        // Before anything else, so every branch below - and every override of the hooks -
        // sees this frame's roster rather than last frame's.
        SyncRoster();

        if (WantsToLeave())
        {
            Log.Info("Net session: leaving");
            Leave(string.Empty);
            return;
        }

        if (Net.IsHost)
        {
            UpdateHost();
            return;
        }

        // `_wasInSession` keeps this on the client branch after the link dies: the
        // framework sets the role back to offline when the host goes away, so testing
        // IsClient alone would silently reroute the drop into the offline branch and the
        // menu would never be reached. NetSession.JoinRequested covers the earlier case
        // still - a join refused before this script ever saw a live session.
        if (Net.IsClient || _wasInSession || _reconnecting || NetSession.JoinRequested)
        {
            UpdateClient(deltaTime);
            return;
        }

        UpdateOffline();
    }

    // ── Game hooks ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Whether the player is asking to leave the session this frame. Escape by default.
    /// </summary>
    /// <remarks>
    /// <para>
    /// A UI element that HANDLED Escape this frame - a text box cancelling an edit - has
    /// already consumed it by the time any script runs, so this reads false on that frame
    /// with nothing to arrange. A game does not need to publish "somebody is typing" for
    /// this to check, and should not: a flag written by one script and read by another is
    /// only right when the two happen to update in the right order.
    /// </para>
    /// <para>
    /// Override it to put something in FRONT of leaving - a pause menu that Escape opens,
    /// with the actual departure behind a Disconnect item. A game that does should return
    /// false here and call <see cref="Leave"/> itself, so exactly one thing owns the key
    /// at any moment.
    /// </para>
    /// </remarks>
    protected virtual bool WantsToLeave() => Input.IsKeyPressed(Key.Escape);

    /// <summary>
    /// Host only: somebody joined, and their display name has arrived. Return false to be
    /// asked again next frame.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The <b>name</b> is what gates this, not a timer. A player prefab is authored with
    /// an empty display name, and the real one is written by the peer that OWNS that
    /// player and then replicated here - which cannot happen until the joiner has been
    /// told it owns anything, so a connection is live and nameless for a handful of
    /// frames. A fixed delay instead would only be a guess that a slow link breaks. A
    /// joiner whose name never arrives at all is never reported, which is the right
    /// failure: the alternative is introducing somebody by a placeholder.
    /// </para>
    /// <para>
    /// The false return is for a game that cannot deliver the news yet. Announcing over
    /// the network needs a carrier with a live script instance on it, and an entity that
    /// was only just spawned does not have one for a frame or two. Notices are held in
    /// order and retried, so nothing is lost and nothing is delivered out of sequence.
    /// </para>
    /// </remarks>
    /// <returns>True once the game has taken the news and this can be forgotten.</returns>
    protected virtual bool AnnouncePlayerJoined(NetSessionPlayer player) => true;

    /// <summary>
    /// Host only: somebody who had been introduced has left. Return false to be asked
    /// again next frame, exactly as for <see cref="AnnouncePlayerJoined"/>.
    /// </summary>
    /// <remarks>
    /// <see cref="NetSessionPlayer.Entity"/> is already invalid - the framework despawns
    /// everything a dropped connection owned before this runs, which is also what takes
    /// the leaver's name tag off every screen. Only somebody whose arrival was reported
    /// is ever reported as leaving: "left" with nobody in front of it names no one.
    /// </remarks>
    protected virtual bool AnnouncePlayerLeft(NetSessionPlayer player) => true;

    // ── Leaving ─────────────────────────────────────────────────────────────────

    /// <summary>Tear the session down and go back to <see cref="ReturnScene"/>, showing
    /// <paramref name="status"/> there if there is anything to explain.</summary>
    /// <remarks>
    /// <para>
    /// <see cref="Net.Disconnect"/> rather than just loading the scene, and the order
    /// matters: it is what gives back every 2D body this peer took off local simulation,
    /// destroys the entities this session spawned rather than leaving them for the next
    /// join to duplicate, and - on the host - tells every client the session ended ON
    /// PURPOSE, so nobody sits retrying a host that just quit. Safe when there is no
    /// session at all, which is what makes leaving work in a solo editor session too.
    /// </para>
    /// <para>
    /// This is a voluntary departure whichever branch reached it, so the reconnect
    /// machinery is cleared out on the way: an attempt still in flight would otherwise
    /// drag the player back into the session they just left.
    /// </para>
    /// </remarks>
    public void Leave(string status)
    {
        Net.Disconnect();
        NetSession.StatusMessage = status;
        NetSession.JoinRequested = false;
        _wasInSession = false;
        _reconnecting = false;
        _attemptLive = false;
        // Nobody has been introduced to a session that no longer exists. Left behind, a
        // rejoin would report only the players who were not in the previous one.
        _introduced.Clear();
        _players.Clear();
        HideStatus();
        if (ReturnScene.Length > 0)
        {
            Scene.Load(ReturnScene);
        }
    }

    // ── Host ────────────────────────────────────────────────────────────────────

    private void UpdateHost()
    {
        if (!_localPlayerSpawned)
        {
            // The host is connection 0 - the id Net.Spawn defaults `owner` to, and the
            // one Net.LocalConnectionId reports here. Its name needs no round trip: it is
            // already on this machine, and the display name is replicated, so writing it
            // locally is what every client ends up reading.
            Entity player = SpawnPlayerFor(Net.LocalConnectionId);
            if (player.IsValid)
            {
                Net.SetPlayerName(player, NetSession.LocalPlayerName);
                LocalPlayer = player;
            }
            _localPlayerSpawned = true;
        }

        uint[] live = Net.Connections;
        foreach (uint connection in live)
        {
            if (!_slotByConnection.ContainsKey(connection))
            {
                SpawnPlayerFor(connection);
            }
        }

        ReleaseDepartedSlots(live);
        DiffRoster();
        FlushNotices();
    }

    /// <summary>Instantiate the replicated player owned by <paramref name="connection"/>
    /// at the first free spawn marker, and remember which marker that was.</summary>
    private Entity SpawnPlayerFor(uint connection)
    {
        int slot = ClaimSlot(connection);
        Entity player = Net.Spawn(PlayerPrefab, SpawnPosition(slot, SharersOf(slot, connection)), connection);
        if (!player.IsValid)
        {
            // Nothing was created, so the slot must not stay claimed or it is lost for the
            // rest of the session.
            _slotOwners[slot] = Unclaimed;
            _slotByConnection.Remove(connection);
            Log.Error($"Net session: could not spawn prefab '{PlayerPrefab}' for connection {connection}");
            return player;
        }
        Log.Info($"Net session: spawned player for connection {connection} at spawn {slot}");
        return player;
    }

    /// <summary>Reclaim the spawn point of any connection that has gone, so the next
    /// joiner does not land on top of somebody.</summary>
    /// <remarks>
    /// Slots only. Who is IN the session is answered by <see cref="SyncRoster"/> from the
    /// world, and a departed connection's player is destroyed by the framework before this
    /// runs, so it has already fallen out of the roster by the time anything here notices.
    /// </remarks>
    private void ReleaseDepartedSlots(uint[] live)
    {
        if (_slotByConnection.Count == 0)
        {
            return;
        }

        List<uint>? departed = null;
        foreach (uint connection in _slotByConnection.Keys)
        {
            // The host's own entry is not in Net.Connections and must never be swept.
            if (connection == Net.LocalConnectionId || System.Array.IndexOf(live, connection) >= 0)
            {
                continue;
            }
            departed ??= new List<uint>();
            departed.Add(connection);
        }
        if (departed == null)
        {
            return;
        }
        foreach (uint connection in departed)
        {
            if (_slotByConnection.Remove(connection, out int slot))
            {
                _slotOwners[slot] = Unclaimed;
            }
            Log.Info($"Net session: connection {connection} left");
        }
    }

    // ── Roster ──────────────────────────────────────────────────────────────────

    /// <summary>Rebuild <see cref="Players"/> from the players this peer is holding.</summary>
    /// <remarks>
    /// <para>
    /// Derived, not accumulated. Every peer holds the session's players as entities - that
    /// is what replication is - so the world answers "who is here" identically on the host
    /// and on a client, while the connection list only answers it on the host. One
    /// derivation is also one thing to be right: a roster built by adding on arrival and
    /// removing on departure has two more places to leak an entry from.
    /// </para>
    /// <para>
    /// A player with no name yet is SKIPPED rather than listed blank. A player prefab is
    /// authored nameless and the real name is written by that player's owner and
    /// replicated here, so a freshly spawned player is nameless for a handful of frames;
    /// listing it would put an empty row in the HUD and introduce somebody as nobody.
    /// </para>
    /// <para>
    /// Ordered by connection, and by entity behind that, so the host is always first and
    /// the order is the join order - <see cref="Net.Players"/> is in world order, which is
    /// not stable and would shuffle the HUD between frames.
    /// </para>
    /// </remarks>
    private void SyncRoster()
    {
        _players.Clear();
        foreach (Entity player in Net.Players)
        {
            string name = Net.GetPlayerName(player);
            if (name.Length == 0)
            {
                continue;
            }
            _players.Add(new NetSessionPlayer(Net.OwnerOf(player), player, name));
        }
        _players.Sort(static (a, b) => a.Connection != b.Connection
            ? a.Connection.CompareTo(b.Connection)
            : a.Entity.Id.CompareTo(b.Entity.Id));
    }

    /// <summary>Host only: turn the difference between the roster and what the game has
    /// already been told into arrival and departure notices.</summary>
    /// <remarks>
    /// The NAME is what gates an arrival, because <see cref="SyncRoster"/> will not list a
    /// player without one - a fixed delay instead would only be a guess that a slow link
    /// breaks. A joiner whose name never arrives at all is never reported, which is the
    /// right failure: the alternative is introducing somebody by a placeholder.
    /// </remarks>
    private void DiffRoster()
    {
        foreach (NetSessionPlayer player in _players)
        {
            if (!_introduced.ContainsKey(player.Connection))
            {
                _introduced[player.Connection] = player;
                _pendingNotices.Add((true, player));
            }
        }

        List<uint>? gone = null;
        foreach (uint connection in _introduced.Keys)
        {
            if (IndexOfPlayer(connection) < 0)
            {
                gone ??= new List<uint>();
                gone.Add(connection);
            }
        }
        if (gone == null)
        {
            return;
        }
        foreach (uint connection in gone)
        {
            NetSessionPlayer player = _introduced[connection];
            _introduced.Remove(connection);
            // The entity is already destroyed, so the departure is reported with an
            // invalid one - the name it carries is the whole point of having kept it.
            _pendingNotices.Add((false, new NetSessionPlayer(connection, default, player.Name)));
        }
    }

    /// <summary>Hand queued arrivals and departures to the game, oldest first, stopping
    /// at the first one it cannot take yet so the order is never scrambled.</summary>
    private void FlushNotices()
    {
        int sent = 0;
        while (sent < _pendingNotices.Count)
        {
            (bool joined, NetSessionPlayer player) = _pendingNotices[sent];
            if (!(joined ? AnnouncePlayerJoined(player) : AnnouncePlayerLeft(player)))
            {
                break;
            }
            sent++;
        }
        if (sent > 0)
        {
            _pendingNotices.RemoveRange(0, sent);
        }
    }

    private int IndexOfPlayer(uint connection)
    {
        for (int i = 0; i < _players.Count; i++)
        {
            if (_players[i].Connection == connection)
            {
                return i;
            }
        }
        return -1;
    }

    /// <summary>The lowest unoccupied spawn marker, or - once every marker is taken - the
    /// one belonging to the oldest index, so an extra player still spawns.</summary>
    private int ClaimSlot(uint connection)
    {
        int slot = System.Array.IndexOf(_slotOwners, Unclaimed);
        if (slot < 0)
        {
            slot = (int)(connection % (uint)_slotOwners.Length);
        }
        _slotOwners[slot] = connection;
        _slotByConnection[connection] = slot;
        return slot;
    }

    /// <summary>Where the marker for <paramref name="index"/> stands, or the origin if
    /// the scene is missing it.</summary>
    /// <remarks>
    /// The <paramref name="sharers"/> offset is what stops players stacking. A scene
    /// provides a fixed number of markers and a session can exceed it - deliberately, since
    /// refusing to spawn the fifth player is worse than crowding the fourth marker - and
    /// two characters created at the identical position are two bodies at the same point,
    /// which physics resolves by shoving one of them somewhere unpredictable. Spreading
    /// them along X by a body width or so makes the overflow land beside the marker instead
    /// of inside whoever is already there. The first claimant of a marker gets it exactly,
    /// so the common case is unaffected.
    /// </remarks>
    private Vector3 SpawnPosition(int index, int sharers = 0)
    {
        Vector3 spread = new(sharers * SpawnSpread, 0.0f, 0.0f);
        string markerName = $"{SpawnPointPrefix}{index}";
        Entity marker = Scene.Find(markerName);
        if (marker.IsValid)
        {
            return marker.Position + spread;
        }
        Log.Warn($"Net session: scene has no '{markerName}' entity; spawning at the origin");
        return spread;
    }

    /// <summary>How many players have already been given <paramref name="slot"/>, not
    /// counting the one asking. 0 whenever there are markers to go round.</summary>
    private int SharersOf(int slot, uint connection)
    {
        int sharers = 0;
        foreach (KeyValuePair<uint, int> entry in _slotByConnection)
        {
            if (entry.Value == slot && entry.Key != connection)
            {
                sharers++;
            }
        }
        return sharers;
    }

    // ── Client ──────────────────────────────────────────────────────────────────

    /// <summary>Watch the link, and decide what its ending means.</summary>
    /// <remarks>
    /// <para>
    /// "Was in a session and is not now" is the whole signal that something ended. There
    /// is no disconnect callback, and the framework does not leave a half-live client
    /// behind to interrogate: losing the link stops the session outright, which drops the
    /// role back to offline and is why <c>_wasInSession</c> has to be latched rather than
    /// <see cref="Net.IsClient"/> tested on its own.
    /// </para>
    /// <para>
    /// The framework's teardown is what removes the replicated entities, so there is no
    /// per-entity cleanup here to write: every player the session spawned - this client's
    /// own included - is destroyed, and any UI their scripts built goes with them.
    /// </para>
    /// </remarks>
    private void UpdateClient(float deltaTime)
    {
        if (Net.IsClient && Net.IsConnected)
        {
            if (_reconnecting)
            {
                Log.Info("Net session: reconnected");
                _reconnecting = false;
                _attemptLive = false;
                ShowNotice("Reconnected.");
            }
            else if (!_wasInSession)
            {
                // Say so once. A level that simply appears is indistinguishable from one
                // that came up offline, and this is a client, so it starts empty until the
                // host's players replicate in - which is exactly the moment a player is
                // most likely to conclude that nothing worked.
                Log.Info("Net session: connected");
                ShowNotice("Connected.");
            }
            _wasInSession = true;
            TickNotice(deltaTime);
            return;
        }

        if (_reconnecting)
        {
            TickReconnect(deltaTime);
            return;
        }

        string reason = Net.DisconnectReason;
        if (reason.Length > 0)
        {
            // Somebody decided. This covers both a host that quit and a join this client
            // was refused outright - including one refused before the session was ever
            // live, which is why this test comes before the _wasInSession one.
            Log.Warn($"Net session: ended - {reason}");
            Leave(reason);
            return;
        }

        if (!_wasInSession)
        {
            // Still connecting for the FIRST time. Nothing has ended, but something has to
            // be watching, or a connection that is never answered waits here forever.
            TickInitialConnect(deltaTime);
            return;
        }

        // Ended with nothing to say: an accident, so try to come back.
        _wasInSession = false;
        BeginReconnect();
    }

    // ── First connection ────────────────────────────────────────────────────────

    /// <summary>Wait for the host to let this client in, and give up out loud rather than
    /// quietly.</summary>
    /// <remarks>
    /// <para>
    /// This covers a game that enters the gameplay scene while the connection is still in
    /// flight. Waiting on the MENU instead is equally supported - set
    /// <see cref="Net.ReplicationReady"/> false before the join and load the level once
    /// <see cref="Net.IsConnected"/> goes true - and in that flow this watchdog simply
    /// never runs, because the session is already live on the first tick here.
    /// </para>
    /// <para>
    /// Two ways to lose: the transport gives up on its own, which drops the role back to
    /// offline and is the only signal an unreachable address produces (there is nobody out
    /// there to send a reason); or nothing happens at all for
    /// <see cref="ConnectTimeoutSeconds"/>. A REFUSAL is not handled here - it arrives with
    /// a reason and is caught by the caller before this runs.
    /// </para>
    /// </remarks>
    private void TickInitialConnect(float deltaTime)
    {
        _connectElapsed += deltaTime;

        if (!Net.IsClient)
        {
            Log.Warn("Net session: the connection was never established");
            Leave(UnreachableMessage);
            return;
        }

        if (_connectElapsed >= ConnectTimeoutSeconds)
        {
            Log.Warn($"Net session: no answer after {ConnectTimeoutSeconds:0} seconds, returning to the menu");
            Leave(ConnectTimedOutMessage);
            return;
        }

        string where = NetSession.HostAddress.Length > 0
            ? $"{NetSession.HostAddress}:{NetSession.HostPort}"
            : "the host";
        ShowStatus($"Connecting to {where}... {_connectElapsed:0.0}s");
    }

    // ── Reconnecting ────────────────────────────────────────────────────────────

    /// <summary>Start the bounded retry sequence after a silent drop.</summary>
    /// <remarks>
    /// Refuses, and goes straight back to the menu, when there is no address to retry - a
    /// player who got here by hosting has nowhere to reconnect TO, and pretending
    /// otherwise would just spend six seconds failing.
    /// </remarks>
    private void BeginReconnect()
    {
        if (NetSession.HostAddress.Length == 0)
        {
            Log.Warn("Net session: link lost and no host address to return to");
            Leave(HostDisconnectedMessage);
            return;
        }
        Log.Warn("Net session: link lost without a reason, attempting to reconnect");
        _reconnecting = true;
        _attemptLive = false;
        _attemptsMade = 0;
        _attemptElapsed = 0.0f;
        // A short wait before the FIRST attempt too: whatever broke the link is rarely
        // fixed by the same millisecond, and an immediate retry mostly just burns one of
        // three attempts.
        _untilNextAttempt = ReconnectDelaySeconds;
        ShowStatus("Connection lost. Reconnecting...");
    }

    /// <summary>One frame of the retry sequence: wait, attempt, judge, repeat.</summary>
    private void TickReconnect(float deltaTime)
    {
        if (_attemptLive)
        {
            string reason = Net.DisconnectReason;
            if (reason.Length > 0)
            {
                // The host is back and does not want us - full, or shutting down. That is
                // an answer, not a failure to retry.
                Leave(reason);
                return;
            }
            _attemptElapsed += deltaTime;
            if (Net.IsClient && _attemptElapsed < ReconnectTimeoutSeconds)
            {
                return; // still in flight
            }
            // Timed out, or the transport gave up on its own. Tidy up before the next one,
            // or a half-open attempt races the one after it.
            Net.Disconnect();
            _attemptLive = false;
            _untilNextAttempt = ReconnectDelaySeconds;
            return;
        }

        if (_attemptsMade >= ReconnectAttempts)
        {
            Log.Warn("Net session: could not reconnect, returning to the menu");
            Leave(HostDisconnectedMessage);
            return;
        }

        _untilNextAttempt -= deltaTime;
        if (_untilNextAttempt > 0.0f)
        {
            return;
        }

        _attemptsMade++;
        _attemptElapsed = 0.0f;
        ShowStatus($"Reconnecting... ({_attemptsMade}/{ReconnectAttempts})");
        if (Net.Connect(NetSession.HostAddress, NetSession.HostPort))
        {
            _attemptLive = true;
            return;
        }
        // Could not even open a socket - count it and wait like any other failure.
        _untilNextAttempt = ReconnectDelaySeconds;
    }

    // ── Status line ─────────────────────────────────────────────────────────────

    /// <summary>Put one line across the top of the screen, creating the element on first
    /// use.</summary>
    /// <remarks>
    /// Built from script rather than authored into the scene because a gameplay scene may
    /// well have no canvas at all, and this line only exists during a failure the scene's
    /// author never sees. UI space has Y=0 at the TOP, and new elements are
    /// centre-anchored, so the anchor is moved to the top edge explicitly.
    /// </remarks>
    private void ShowStatus(string text)
    {
        if (!_status.IsValid)
        {
            _status = Ui.CreateText();
            Vector2 topCentre = new(0.5f, 0.0f);
            Ui.SetAnchors(_status, topCentre, topCentre);
            Ui.SetPivot(_status, new Vector2(0.5f, 0.0f));
            Ui.SetRect(_status, 0.0f, 24.0f, 520.0f, 28.0f);
            Ui.SetTextAlign(_status, UiHAlign.Center, UiVAlign.Middle);
            Ui.SetTextColor(_status, new Vector4(1.0f, 0.86f, 0.45f, 1.0f));
        }
        Ui.SetText(_status, text);
    }

    private void HideStatus()
    {
        _noticeLeft = 0.0f;
        if (_status.IsValid)
        {
            _status.Destroy();
            _status = default;
        }
    }

    /// <summary>Put a line up that takes itself down again. For things that are worth
    /// saying once - "Connected." - rather than a condition that persists.</summary>
    private void ShowNotice(string text)
    {
        ShowStatus(text);
        _noticeLeft = NoticeSeconds;
    }

    private void TickNotice(float deltaTime)
    {
        if (_noticeLeft <= 0.0f)
        {
            return;
        }
        _noticeLeft -= deltaTime;
        if (_noticeLeft <= 0.0f)
        {
            HideStatus();
        }
    }

    // ── Offline ─────────────────────────────────────────────────────────────────

    /// <summary>No session at all - the scene opened straight from the editor. Put one
    /// player in it so the level stays playable solo. <see cref="Scene.Instantiate"/>
    /// rather than <see cref="Net.Spawn"/>, which is host-only and would return nothing
    /// here.</summary>
    private void UpdateOffline()
    {
        if (_localPlayerSpawned)
        {
            return;
        }
        _localPlayerSpawned = true;
        int slot = ClaimSlot(Net.LocalConnectionId);
        Entity player = Scene.Instantiate(PlayerPrefab, SpawnPosition(slot, SharersOf(slot, Net.LocalConnectionId)));
        if (!player.IsValid)
        {
            return;
        }
        Net.SetPlayerName(player, NetSession.LocalPlayerName);
        LocalPlayer = player;
        // Nothing is added to the roster here: SyncRoster picks this player up on the next
        // frame like any other. And nothing is ANNOUNCED - DiffRoster is host-only, because
        // there is nobody to announce a solo game to and a game telling itself that it
        // joined reads as a bug.
    }
}
