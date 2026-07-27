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
/// On a <b>client</b> this does almost nothing: the players arrive by replication,
/// and the one job left is noticing that the host went away and returning to the
/// title screen with something to show for it. A client's own name is reported by
/// <see cref="PlayerController"/> rather than from here - see the note on
/// <see cref="PlayerController.SubmitName"/> for why it cannot be sent from this
/// entity.
/// </para>
/// <para>
/// There is no join/leave callback in the framework, so the host polls
/// <see cref="Net.Connections"/> and diffs it. That is cheap (a handful of ids) and
/// is the only signal available. The matching client-side signal is
/// <see cref="Net.IsConnected"/> going false, which is what a dropped host looks
/// like from the other end.
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

    /// <summary>What <see cref="StatusMessage"/> is set to when the host goes away.
    /// A constant because it is the one status a test can assert on verbatim.</summary>
    public const string HostDisconnectedMessage = "Host disconnected";

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

    /// <inheritdoc/>
    public override void OnAttach()
    {
        _slotOwners = new uint[System.Math.Max(SpawnPointCount, 1)];
        for (int i = 0; i < _slotOwners.Length; i++)
        {
            _slotOwners[i] = Unclaimed;
        }
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (Net.IsHost)
        {
            UpdateHost();
            return;
        }

        // `_wasInSession` keeps this on the client branch after the link dies: the
        // framework sets the role back to offline when the host goes away, so testing
        // IsClient alone would silently reroute the drop into the offline branch and
        // the title screen would never be reached.
        if (Net.IsClient || _wasInSession)
        {
            UpdateClient();
            return;
        }

        UpdateOffline();
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
    /// with an empty <c>NetPlayer.displayName</c> and the host writes the real one only
    /// when <see cref="PlayerController.SubmitName"/> arrives - an RPC the joiner can
    /// only send once its player has spawned and it has been told it owns it, so a
    /// connection is live and nameless for a handful of frames. Announcing the moment
    /// the connection appears in <see cref="Net.Connections"/> therefore prints an
    /// empty name; a fixed delay would only be a guess that a slow link breaks.
    /// </para>
    /// <para>
    /// A non-empty name is the exact signal, with no ambiguity to work around:
    /// <see cref="PlayerController.SubmitName"/> is the only thing that ever writes the
    /// field, and it substitutes "Player" for a blank rather than storing one, so
    /// "still empty" and "genuinely called Player" cannot be confused. A joiner whose
    /// name never arrives at all is simply never announced, which is the right failure
    /// - the alternative is introducing somebody by a placeholder.
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
    /// gate that only lets a client drive what it owns (see
    /// <see cref="PlayerController.SubmitName"/>) - a client-owned player would happen
    /// to work for a host-originated multicast, but reaching for one out of habit is
    /// exactly the mistake that gate punishes elsewhere. It carries the
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

    /// <summary>Watch for the host going away, and leave the arena when it does.</summary>
    /// <remarks>
    /// <para>
    /// "Was in a session and is not now" is the whole signal. There is no disconnect
    /// callback either, and the framework does not leave a half-live client behind to
    /// interrogate: losing the link stops the session outright, which drops the role
    /// back to offline and is why <c>_wasInSession</c> has to be latched rather than
    /// <see cref="Net.IsClient"/> tested on its own.
    /// </para>
    /// <para>
    /// That same teardown is what removes the replicated entities, so there is no
    /// per-entity cleanup here to write: every player the session spawned - this
    /// client's own included - is destroyed, and their name tags and chat UI go with
    /// them through <see cref="NameTag.OnDetach"/> and <see cref="ChatBox.OnDetach"/>.
    /// What is left is an arena with nobody in it, and the only job is to not sit in
    /// it. No host migration: see the class remarks.
    /// </para>
    /// </remarks>
    private void UpdateClient()
    {
        if (Net.IsClient && Net.IsConnected)
        {
            _wasInSession = true;
            return;
        }

        if (!_wasInSession)
        {
            // Either still connecting, or this arena is being played offline from the
            // editor - neither is a dropped host.
            return;
        }

        _wasInSession = false;
        StatusMessage = HostDisconnectedMessage;
        Debug.LogWarning("Whisper: host disconnected, returning to the title screen");
        Scene.Load("Title");
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
