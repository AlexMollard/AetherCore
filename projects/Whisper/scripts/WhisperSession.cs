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
/// back.
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
/// is the only signal available.
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
        ReleaseDepartedSlots(live);
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
        Debug.Log($"Whisper: spawned player for connection {connection} at spawn {slot}");
        return player;
    }

    /// <summary>Forget connections that have gone. Their player entities are already
    /// destroyed - the framework despawns everything a dropped connection owned - so
    /// this only reclaims the spawn point for the next joiner.</summary>
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
            Debug.Log($"Whisper: connection {connection} left");
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

        // The link died: the framework tears the whole session down on a client when
        // the host goes away, so "was in a session and is not now" is the signal.
        // Task 9 hardens this; a plain return to the title screen is enough here.
        _wasInSession = false;
        StatusMessage = "Lost connection to the host.";
        Debug.LogWarning("Whisper: host connection lost, returning to the title screen");
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
