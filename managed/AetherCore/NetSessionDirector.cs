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

    // The entity spawned for each known connection, so a pending join can poll that
    // player's replicated display name without going looking for it by name.
    private readonly Dictionary<uint, Entity> _playerByConnection = new();

    // Connections whose player exists but whose display name has not arrived yet, in
    // arrival order so several landing on one frame are still introduced in the order the
    // people actually turned up. See RegisterNamedArrivals for why the name gates it.
    private readonly List<uint> _awaitingName = new();

    // Everyone currently in the session, in the order they were introduced. Doubling as
    // the "has been introduced" set is deliberate: only somebody the game was told about
    // can be said to have left.
    private readonly List<NetSessionPlayer> _players = new();

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

    /// <summary>Everyone in the session, host first, in the order they were introduced.
    /// Empty on a client - see <see cref="Net.Connections"/> for why a client is told
    /// nothing about its peers.</summary>
    public IReadOnlyList<NetSessionPlayer> Players => _players;

    /// <summary>True while the bounded reconnect sequence is running.</summary>
    public bool IsReconnecting => _reconnecting;

    /// <inheritdoc/>
    public override void OnAttach()
    {
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
    /// Override to suppress it while something else owns the keyboard - a chat box uses
    /// Escape to cancel an edit, and a key that both cancels a message and quits the
    /// session makes the chat unusable. The usual shape is
    /// <c>=&gt; !MyChat.IsTyping &amp;&amp; base.WantsToLeave();</c>.
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

        // Departures are swept BEFORE arrivals are registered, and the order is load
        // bearing. A connection that has already gone had its player entity destroyed by
        // the framework before this ran, and RegisterNamedArrivals reads a display name
        // straight off those entities - clearing the dead ones out first is what stops it
        // reading through a handle to something that is not there.
        ReleaseDepartedSlots(live);
        RegisterNamedArrivals();
        FlushNotices();
    }

    /// <summary>Instantiate the replicated player owned by <paramref name="connection"/>
    /// at the first free spawn marker, and remember which marker that was.</summary>
    private Entity SpawnPlayerFor(uint connection)
    {
        int slot = ClaimSlot(connection);
        Entity player = Net.Spawn(PlayerPrefab, SpawnPosition(slot), connection);
        if (!player.IsValid)
        {
            // Nothing was created, so the slot must not stay claimed or it is lost for the
            // rest of the session.
            _slotOwners[slot] = Unclaimed;
            _slotByConnection.Remove(connection);
            Log.Error($"Net session: could not spawn prefab '{PlayerPrefab}' for connection {connection}");
            return player;
        }
        _playerByConnection[connection] = player;
        _awaitingName.Add(connection);
        Log.Info($"Net session: spawned player for connection {connection} at spawn {slot}");
        return player;
    }

    /// <summary>Forget connections that have gone: reclaim the spawn point for the next
    /// joiner, drop the per-connection bookkeeping, and queue the departure.</summary>
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
            _playerByConnection.Remove(connection);
            _awaitingName.Remove(connection);
            int index = IndexOfPlayer(connection);
            if (index >= 0)
            {
                // The entity is already destroyed, so the roster entry is reported with an
                // invalid one - the name it carries is the whole point of having kept it.
                NetSessionPlayer player = _players[index];
                _players.RemoveAt(index);
                _pendingNotices.Add((false, new NetSessionPlayer(connection, default, player.Name)));
            }
            // No else: a connection that dropped before its name ever reached us was never
            // introduced, and there is nothing honest to say about it.
            Log.Info($"Net session: connection {connection} left");
        }
    }

    /// <summary>Admit every pending arrival whose display name has landed to the roster,
    /// and give up on any whose player went away first.</summary>
    private void RegisterNamedArrivals()
    {
        // Forward, with a manual index, so removing an entry does not reorder the ones
        // behind it - a reverse sweep would report a frame's arrivals backwards.
        int i = 0;
        while (i < _awaitingName.Count)
        {
            uint connection = _awaitingName[i];
            if (!_playerByConnection.TryGetValue(connection, out Entity player))
            {
                _awaitingName.RemoveAt(i);
                continue;
            }
            string name = Net.GetPlayerName(player);
            if (name.Length == 0)
            {
                i++;
                continue;
            }
            _awaitingName.RemoveAt(i);
            NetSessionPlayer entry = new(connection, player, name);
            _players.Add(entry);
            _pendingNotices.Add((true, entry));
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
    private Vector3 SpawnPosition(int index)
    {
        string markerName = $"{SpawnPointPrefix}{index}";
        Entity marker = Scene.Find(markerName);
        if (marker.IsValid)
        {
            return marker.Position;
        }
        Log.Warn($"Net session: scene has no '{markerName}' entity; spawning at the origin");
        return Vector3.Zero;
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
    /// This runs in the GAMEPLAY scene, not the menu, and that is forced rather than
    /// chosen: the host's join replay sends the Welcome and the Spawn for every entity
    /// already in the session back-to-back on the same reliable channel, so they arrive in
    /// one receive pass. A client that was still sitting on a menu "watching the
    /// connection" would create every one of those entities into the MENU scene and then
    /// destroy them all on the scene change, and the host - which tracks what it has
    /// already sent - would never send them again. The client has to be standing in the
    /// level before it is let in, so the level is where the waiting is shown.
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
        Entity player = Scene.Instantiate(PlayerPrefab, SpawnPosition(ClaimSlot(Net.LocalConnectionId)));
        if (!player.IsValid)
        {
            return;
        }
        Net.SetPlayerName(player, NetSession.LocalPlayerName);
        LocalPlayer = player;
        // On the roster, but never announced: there is nobody to announce it to, and a
        // solo game telling itself that it joined reads as a bug.
        _players.Add(new NetSessionPlayer(Net.LocalConnectionId, player, Net.GetPlayerName(player)));
    }
}
