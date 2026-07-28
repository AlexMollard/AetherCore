using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The arena's session director, and the home of the session-wide state that has to
/// outlive a scene load.
/// </summary>
/// <remarks>
/// <para>
/// On the <b>host</b> this owns the player population: one <c>player</c> prefab for
/// the host itself the moment the arena starts, one more for every connection that
/// joins, each at its own <c>Spawn{n}</c> marker. A leaver's player is despawned by
/// the framework before this script next ticks (every entity a dropped connection
/// owned goes with it), so the only thing left to do here is hand its spawn point
/// back and say who went. The host is also the only peer that announces anything:
/// arrivals and departures are pushed to every transcript through
/// <see cref="ChatBox.Announce"/>, the same multicast the chat itself rides.
/// </para>
/// <para>
/// On a <b>client</b> this does almost nothing while things are going well: the
/// players arrive by replication, and this peer's own name is written by
/// <see cref="PlayerController"/> onto the player it owns, because the owner of an
/// entity is authoritative for it. What is left is deciding what a link ENDING means.
/// </para>
/// <para>
/// <b>Not every ending is the same, and treating them alike is what makes a session
/// feel broken.</b> A host that quit, and a host this client was refused by, both
/// arrive with a reason attached (<see cref="Net.DisconnectReason"/>): somebody
/// decided, and there is nothing to retry. A link that simply stopped - a timeout, a
/// dropped packet storm, a laptop lid - arrives with nothing to say, and that silence
/// is the signal to try coming back. So this reconnects a bounded number of times on
/// the second kind and never on the first, and a player who pressed Escape is not
/// dragged back into a session they just left.
/// </para>
/// <para>
/// There is no join/leave callback in the framework, so the host polls
/// <see cref="Net.Connections"/> and diffs it. That is cheap (a handful of ids) and
/// is the only signal available. The matching client-side signal is
/// <see cref="Net.IsConnected"/> going false.
/// </para>
/// <para>
/// <b>There is no host migration.</b> When the host goes, the session goes: every
/// client tears its replicated entities down and returns to the title screen. That
/// is a deliberate non-goal, not an unfinished edge - promoting a client would mean
/// re-deriving the whole authoritative world on a peer that only ever saw snapshots
/// of it.
/// </para>
/// </remarks>
public sealed class WhisperSession : EntityScript
{
    /// <summary>Name this player typed on the connect screen; "Player" if blank.
    /// Static so it survives the Title -> Arena scene load that discards every
    /// entity, including the one that collected it.</summary>
    public static string LocalPlayerName = "Player";

    /// <summary>Why the last session ended, shown on the title screen after an
    /// involuntary return. Empty when the title screen was reached normally.</summary>
    public static string StatusMessage = "";

    /// <summary>Address of the host this player last asked to join, so a dropped link
    /// knows where to try coming back to. Static for the same reason
    /// <see cref="LocalPlayerName"/> is: the connect screen that collected it is
    /// destroyed by the scene load into the arena.</summary>
    public static string HostAddress = "";

    /// <summary>Port half of <see cref="HostAddress"/>.</summary>
    public static ushort HostPort;

    /// <summary>
    /// True from the moment the connect screen asks to join until the arena is left.
    /// </summary>
    /// <remarks>
    /// It exists for one case that no engine state can express: a join REFUSED before
    /// this script first ticks. The framework tears the session down as soon as the
    /// refusal lands, so <see cref="Net.IsClient"/> is already false and the arena would
    /// otherwise look exactly like a single-player editor session and spawn a solo
    /// player into a server it was just thrown out of. Reset in <see cref="OnAttach"/>
    /// whenever nothing is live and nothing has anything to say, because a static
    /// survives the editor's Play/Stop cycle and a stale one would route a genuine
    /// single-player session down the client branch.
    /// </remarks>
    public static bool JoinRequested;

    /// <summary>What <see cref="StatusMessage"/> is set to when the host goes away
    /// without a word and reconnecting has run out of attempts. A constant because it
    /// is the one status a test can assert on verbatim.</summary>
    public const string HostDisconnectedMessage = "Host disconnected";

    /// <summary>Players in one session, the host included. Whisper's number, not the
    /// framework's: <see cref="Net.Host"/> caps CONNECTIONS, and the host is not one of
    /// its own, so it hosts with one fewer than this.</summary>
    public const int MaxPlayers = 4;

    /// <summary>How many times a silent drop is retried before giving up. Bounded, and
    /// small: three tries over roughly six seconds is long enough to ride out a hiccup
    /// and short enough that a player staring at a dead arena is sent somewhere useful
    /// rather than left hoping.</summary>
    public const int ReconnectAttempts = 3;

    /// <summary>Seconds between reconnect attempts.</summary>
    public const float ReconnectDelaySeconds = 2.0f;

    /// <summary>Seconds a single reconnect attempt is given before it counts as
    /// failed. A connect that is never answered would otherwise hang the whole retry
    /// sequence on the first attempt.</summary>
    public const float ReconnectTimeoutSeconds = 5.0f;

    /// <summary>Prefab spawned for each player. A bare stem, not a path - that is
    /// what <see cref="Net.Spawn"/> takes, and the name travels on the wire.</summary>
    public string PlayerPrefab = "player";

    /// <summary>How many <c>Spawn{n}</c> markers the arena provides. Players beyond
    /// this many share, which is better than refusing to spawn them.</summary>
    public int SpawnPointCount = 4;

    // Which connection is standing on each spawn point, so two players never land on
    // top of each other. kUnclaimed rather than 0, because 0 is the HOST's connection
    // id - it is a real occupant, not an empty slot.
    private const uint Unclaimed = uint.MaxValue;
    private uint[] _slotOwners = [];

    // Connections seen last frame, to diff Net.Connections against. Also the map from
    // a connection to the player it was given, so a departure frees the right slot.
    private readonly Dictionary<uint, int> _slotByConnection = new();

    // The player entity spawned for each known connection, so a pending join can poll
    // that player's replicated display name without going looking for it by name.
    private readonly Dictionary<uint, Entity> _playerByConnection = new();

    // Connections whose player exists but whose display name has not arrived yet, in
    // arrival order so several landing on one frame are still announced in the order
    // the people actually turned up. See AnnounceNamedJoins for why the name gates it.
    private readonly List<uint> _awaitingName = new();

    // The name each already-announced connection joined under. Kept because a leaver's
    // player entity is destroyed by the framework BEFORE this script next ticks, so by
    // the time there is a departure to announce there is nothing left to read a name
    // off. This dictionary doubling as the "was announced" set is deliberate: only
    // somebody the transcript introduced can be said to have left it.
    private readonly Dictionary<uint, string> _nameByConnection = new();

    // Announcement lines waiting for a carrier that can actually deliver them, oldest
    // first. See FlushAnnouncements.
    private readonly List<string> _pendingAnnouncements = new();

    // The host's own player - the entity every announcement rides. See FlushAnnouncements.
    private Entity _hostPlayer;

    private bool _hostPlayerSpawned;
    private bool _wasInSession;

    // Reconnect state. `_reconnecting` is the mode; the other three are one attempt's
    // worth of bookkeeping.
    private bool _reconnecting;
    private bool _attemptLive;
    private int _attemptsMade;
    private float _attemptElapsed;
    private float _untilNextAttempt;

    // On-screen line for reconnect progress. Created only when there is something to
    // say - an empty arena with a dead link is exactly when the player has no other
    // source of information about what is happening.
    private Entity _status;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        _slotOwners = new uint[System.Math.Max(SpawnPointCount, 1)];
        for (int i = 0; i < _slotOwners.Length; i++)
        {
            _slotOwners[i] = Unclaimed;
        }

        // Nothing live and nothing to report: this arena was opened straight from the
        // editor. See JoinRequested for why a stale static has to be cleared here.
        if (!Net.IsClient && !Net.IsHost && !Net.IsConnected && Net.DisconnectReason.Length == 0)
        {
            JoinRequested = false;
        }
    }

    /// <inheritdoc/>
    public override void OnDetach()
    {
        HideStatus();
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (HandleLeaveRequest())
        {
            return;
        }

        if (Net.IsHost)
        {
            UpdateHost();
            return;
        }

        // `_wasInSession` keeps this on the client branch after the link dies: the
        // framework sets the role back to offline when the host goes away, so testing
        // IsClient alone would silently reroute the drop into the offline branch and
        // the title screen would never be reached. `JoinRequested` covers the earlier
        // case still - a join refused before this script ever saw a live session.
        if (Net.IsClient || _wasInSession || _reconnecting || JoinRequested)
        {
            UpdateClient(deltaTime);
            return;
        }

        UpdateOffline();
    }

    // ── Leaving ─────────────────────────────────────────────────────────────────

    /// <summary>Escape leaves the arena. Returns true when it did, so the caller stops
    /// updating a session that is on its way out.</summary>
    /// <remarks>
    /// Suppressed while the chat box has the keyboard: the text box uses Escape to
    /// cancel an edit, and a key that both cancels a message and quits the session
    /// would make the chat unusable.
    /// </remarks>
    private bool HandleLeaveRequest()
    {
        if (ChatBox.LocalIsTyping || !Input.IsKeyPressed(Key.Escape))
        {
            return false;
        }
        Debug.Log("Whisper: leaving the arena");
        LeaveArena("");
        return true;
    }

    /// <summary>Tear the session down and go back to the title screen, showing
    /// <paramref name="status"/> there if there is anything to explain.</summary>
    /// <remarks>
    /// <para>
    /// <see cref="Net.Disconnect"/> rather than just loading the scene, and the order
    /// matters: it is what gives back every 2D body this peer took off local simulation,
    /// destroys the entities this session spawned rather than leaving them for the next
    /// join to duplicate, and - on the host - tells every client the session ended ON
    /// PURPOSE, so nobody sits retrying a host that just quit. Safe when there is no
    /// session at all, which is what makes Escape work in a solo editor session too.
    /// </para>
    /// <para>
    /// This is a voluntary departure whichever branch reached it, so the reconnect
    /// machinery is cleared out on the way: an attempt still in flight would otherwise
    /// drag the player back into the session they just left.
    /// </para>
    /// </remarks>
    private void LeaveArena(string status)
    {
        Net.Disconnect();
        StatusMessage = status;
        JoinRequested = false;
        _wasInSession = false;
        _reconnecting = false;
        _attemptLive = false;
        HideStatus();
        Scene.Load("Title");
    }

    // ── Host ────────────────────────────────────────────────────────────────────

    private void UpdateHost()
    {
        if (!_hostPlayerSpawned)
        {
            // The host is connection 0 - the id Net.Spawn defaults `owner` to, and the
            // one Net.LocalConnectionId reports here. Its name needs no round trip: it
            // is already on this machine, and NetPlayer.displayName is replicated, so
            // writing it locally is what every client ends up reading.
            Entity player = SpawnPlayerFor(Net.LocalConnectionId);
            if (player.IsValid)
            {
                Net.SetPlayerName(player, LocalPlayerName);
                _hostPlayer = player;
            }
            _hostPlayerSpawned = true;
        }

        uint[] live = Net.Connections;
        foreach (uint connection in live)
        {
            if (!_slotByConnection.ContainsKey(connection))
            {
                SpawnPlayerFor(connection);
            }
        }

        // Departures are swept BEFORE joins are announced, and the order is load
        // bearing. A connection that has already gone had its player entity destroyed
        // by the framework before this script ran, and AnnounceNamedJoins reads a
        // display name straight off those entities - clearing the dead ones out first
        // is what stops it reading through a handle to something that is not there.
        ReleaseDepartedSlots(live);
        AnnounceNamedJoins();
        FlushAnnouncements();
    }

    /// <summary>Instantiate the replicated player owned by <paramref name="connection"/>
    /// at the first free spawn marker, and remember which marker that was.</summary>
    private Entity SpawnPlayerFor(uint connection)
    {
        int slot = ClaimSlot(connection);
        Entity player = Net.Spawn(PlayerPrefab, SpawnPosition(slot), connection);
        if (!player.IsValid)
        {
            // Nothing was created, so the slot must not stay claimed or it is lost for
            // the rest of the session.
            _slotOwners[slot] = Unclaimed;
            _slotByConnection.Remove(connection);
            Debug.LogError($"Whisper: could not spawn prefab '{PlayerPrefab}' for connection {connection}");
            return player;
        }
        _playerByConnection[connection] = player;
        _awaitingName.Add(connection);
        Debug.Log($"Whisper: spawned player for connection {connection} at spawn {slot}");
        return player;
    }

    /// <summary>
    /// Forget connections that have gone: reclaim the spawn point for the next joiner,
    /// drop the per-connection bookkeeping, and queue the leave announcement.
    /// </summary>
    /// <remarks>
    /// Their player entities are already destroyed - the framework despawns everything
    /// a dropped connection owned, which is also what takes the leaver's name tag off
    /// every screen - so nothing here touches an entity. The name being announced is
    /// the one cached at join time for exactly this reason.
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
            _playerByConnection.Remove(connection);
            _awaitingName.Remove(connection);
            if (_nameByConnection.Remove(connection, out string? name))
            {
                _pendingAnnouncements.Add($"{name} left");
            }
            // No else: a connection that dropped before its name ever reached us was
            // never introduced to the transcript, and "left" with nobody in front of it
            // names no one. Silence is the honest answer there.
            Debug.Log($"Whisper: connection {connection} left");
        }
    }

    /// <summary>Queue a join line for every pending arrival whose display name has
    /// landed, and give up on any whose player went away first.</summary>
    /// <remarks>
    /// <para>
    /// The <b>name</b> is what gates this, not a timer. A player prefab is authored
    /// with an empty <c>NetPlayer.displayName</c>, and the real one is written by the
    /// peer that OWNS that player and then replicated here - which cannot happen until
    /// the joiner has been told it owns anything, so a connection is live and nameless
    /// for a handful of frames. Announcing the moment the connection appears in
    /// <see cref="Net.Connections"/> therefore prints an empty name; a fixed delay
    /// would only be a guess that a slow link breaks.
    /// </para>
    /// <para>
    /// A non-empty name is the exact signal, with no ambiguity to work around:
    /// <see cref="Net.ClaimPlayerName"/> is the only thing that ever writes the field,
    /// and it substitutes "Player" for a blank rather than storing one, so "still
    /// empty" and "genuinely called Player" cannot be confused. A joiner whose name
    /// never arrives at all is simply never announced, which is the right failure - the
    /// alternative is introducing somebody by a placeholder.
    /// </para>
    /// </remarks>
    private void AnnounceNamedJoins()
    {
        // Forward, with a manual index, so removing an entry does not reorder the ones
        // behind it - a reverse sweep would announce a frame's arrivals backwards.
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
            _nameByConnection[connection] = name;
            _pendingAnnouncements.Add($"{name} joined");
        }
    }

    /// <summary>Hand queued announcements to the transcript, oldest first, stopping at
    /// the first one that cannot be routed yet so the order is never scrambled.</summary>
    /// <remarks>
    /// <para>
    /// Every announcement rides the <b>host's own player</b>, and that carrier is
    /// chosen on three counts. It is host-owned, so it is nowhere near the ownership
    /// gate that only lets a peer drive what it owns - a client-owned player would
    /// happen to work for a host-originated multicast, but reaching for one out of
    /// habit is exactly the mistake that gate punishes elsewhere. It carries the
    /// <see cref="ChatBox"/> script that declares the multicast. And it outlives every
    /// client in the session, which a <i>leave</i> announcement needs: the leaver's own
    /// player is already destroyed by the time there is anything to say about it.
    /// </para>
    /// <para>
    /// <see cref="ChatBox.Announce"/> refuses rather than pretends whenever the carrier
    /// cannot deliver yet - most obviously on the frame the host's player is spawned,
    /// when the entity exists but its script instance does not - so the queue drains on
    /// a later frame instead of a line being silently dropped.
    /// </para>
    /// </remarks>
    private void FlushAnnouncements()
    {
        int sent = 0;
        while (sent < _pendingAnnouncements.Count && ChatBox.Announce(_hostPlayer, _pendingAnnouncements[sent]))
        {
            sent++;
        }
        if (sent > 0)
        {
            _pendingAnnouncements.RemoveRange(0, sent);
        }
    }

    /// <summary>The lowest unoccupied spawn marker, or - once every marker is taken -
    /// the one belonging to the oldest index, so a fifth player still spawns.</summary>
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

    /// <summary>Where <c>Spawn{index}</c> stands, or the arena origin if the scene is
    /// missing that marker.</summary>
    private Vector3 SpawnPosition(int index)
    {
        Entity marker = Scene.Find($"Spawn{index}");
        if (marker.IsValid)
        {
            return marker.Position;
        }
        Debug.LogWarning($"Whisper: arena has no 'Spawn{index}' entity; spawning at the origin");
        return Vector3.Zero;
    }

    // ── Client ──────────────────────────────────────────────────────────────────

    /// <summary>Watch the link, and decide what its ending means.</summary>
    /// <remarks>
    /// <para>
    /// "Was in a session and is not now" is the whole signal that something ended.
    /// There is no disconnect callback, and the framework does not leave a half-live
    /// client behind to interrogate: losing the link stops the session outright, which
    /// drops the role back to offline and is why <c>_wasInSession</c> has to be latched
    /// rather than <see cref="Net.IsClient"/> tested on its own.
    /// </para>
    /// <para>
    /// WHY it ended is a separate question, and <see cref="Net.DisconnectReason"/> is
    /// the only thing that can answer it: the framework fills it in when the far end
    /// ended the link deliberately and said so - a refusal, a host closing the session -
    /// and leaves it empty for every accidental ending. Retrying a deliberate ending
    /// gets the same answer and looks broken to the player who was just thrown out;
    /// giving up on an accidental one throws away a session over one bad second.
    /// </para>
    /// <para>
    /// The framework's teardown is what removes the replicated entities, so there is no
    /// per-entity cleanup here to write: every player the session spawned - this
    /// client's own included - is destroyed, and their name tags and chat UI go with
    /// them through <see cref="NameTag.OnDetach"/> and <see cref="ChatBox.OnDetach"/>.
    /// No host migration: see the class remarks.
    /// </para>
    /// </remarks>
    private void UpdateClient(float deltaTime)
    {
        if (Net.IsClient && Net.IsConnected)
        {
            _wasInSession = true;
            if (_reconnecting)
            {
                Debug.Log("Whisper: reconnected");
                _reconnecting = false;
                _attemptLive = false;
                HideStatus();
            }
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
            // Somebody decided. This covers both a host that quit and a join this
            // client was refused outright - including one refused before the session
            // was ever live, which is why this test comes before the _wasInSession one.
            Debug.LogWarning($"Whisper: session ended - {reason}");
            LeaveArena(reason);
            return;
        }

        if (!_wasInSession)
        {
            // Still connecting for the first time. Nothing has ended, so there is
            // nothing to react to yet.
            return;
        }

        // Ended with nothing to say: an accident, so try to come back.
        _wasInSession = false;
        BeginReconnect();
    }

    // ── Reconnecting ────────────────────────────────────────────────────────────

    /// <summary>Start the bounded retry sequence after a silent drop.</summary>
    /// <remarks>
    /// Refuses, and goes straight back to the title screen, when there is no address to
    /// retry - a player who reached the arena by hosting has nowhere to reconnect TO,
    /// and pretending otherwise would just spend six seconds failing.
    /// </remarks>
    private void BeginReconnect()
    {
        if (HostAddress.Length == 0)
        {
            Debug.LogWarning("Whisper: link lost and no host address to return to");
            LeaveArena(HostDisconnectedMessage);
            return;
        }
        Debug.LogWarning("Whisper: link lost without a reason, attempting to reconnect");
        _reconnecting = true;
        _attemptLive = false;
        _attemptsMade = 0;
        _attemptElapsed = 0.0f;
        // A short wait before the FIRST attempt too: whatever broke the link is rarely
        // fixed by the same millisecond, and an immediate retry mostly just burns one
        // of three attempts.
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
                // The host is back and does not want us - full, or shutting down. That
                // is an answer, not a failure to retry.
                LeaveArena(reason);
                return;
            }
            _attemptElapsed += deltaTime;
            if (Net.IsClient && _attemptElapsed < ReconnectTimeoutSeconds)
            {
                return; // still in flight
            }
            // Timed out, or the transport gave up on its own. Tidy up before the next
            // one, or a half-open attempt races the one after it.
            Net.Disconnect();
            _attemptLive = false;
            _untilNextAttempt = ReconnectDelaySeconds;
            return;
        }

        if (_attemptsMade >= ReconnectAttempts)
        {
            Debug.LogWarning("Whisper: could not reconnect, returning to the title screen");
            LeaveArena(HostDisconnectedMessage);
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
        if (Net.Connect(HostAddress, HostPort))
        {
            _attemptLive = true;
            return;
        }
        // Could not even open a socket - count it and wait like any other failure.
        _untilNextAttempt = ReconnectDelaySeconds;
    }

    // ── Status line ─────────────────────────────────────────────────────────────

    /// <summary>Put one line across the top of the screen, creating the element on
    /// first use.</summary>
    /// <remarks>
    /// Built from script rather than authored into the arena for the same reason
    /// <see cref="NameTag"/>'s label is: the scene has no canvas, and passing a
    /// scene-placed element to a script that has to work in a prefab-free scene is more
    /// wiring than the one line is worth. UI space has Y=0 at the TOP, and new elements
    /// are centre-anchored, so the anchor is moved to the top edge explicitly.
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
        if (_status.IsValid)
        {
            _status.Destroy();
            _status = default;
        }
    }

    // ── Offline ─────────────────────────────────────────────────────────────────

    /// <summary>No session at all - the arena opened straight from the editor. Put one
    /// player in it so the level stays playable solo; <see cref="Net.IsOwner"/> is true
    /// offline, so <see cref="PlayerController"/> drives it exactly as it would online.
    /// <see cref="Scene.Instantiate"/> rather than <see cref="Net.Spawn"/>, which is
    /// host-only and would return nothing here.</summary>
    private void UpdateOffline()
    {
        if (_hostPlayerSpawned)
        {
            return;
        }
        _hostPlayerSpawned = true;
        Entity player = Scene.Instantiate(PlayerPrefab, SpawnPosition(ClaimSlot(Net.LocalConnectionId)));
        if (player.IsValid)
        {
            Net.SetPlayerName(player, LocalPlayerName);
        }
    }
}
