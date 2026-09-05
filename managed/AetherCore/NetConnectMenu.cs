namespace AetherCore;

/// <summary>
/// The connect ceremony every multiplayer menu currently hand-rolls: mint or accept a
/// room code, wait out the connect ladder, flip <see cref="Net.ReplicationReady"/> at
/// exactly the right moment, and hand off to the gameplay scene once - and only once -
/// the session is genuinely live.
/// </summary>
/// <remarks>
/// <para>
/// Attach one to an entity in the menu scene and wire the game's own UI to call
/// <see cref="Host"/> or <see cref="Join"/> - a key press, a button click, whatever the
/// screen already does. This narrates the wait on <see cref="StatusText"/> and loads
/// <see cref="ArenaScene"/> once it is safe to. <see cref="NetSessionDirector"/> is the
/// other half of this pair and is unaffected by anything here: it still has to be
/// attached to a scene entity in the gameplay scene, still spawns the player prefab per
/// connection at its spawn markers, and still spawns one player when that scene is
/// opened with no session at all. Games that build their own connect screen from
/// <see cref="NetSession"/> directly keep working unchanged - this is a convenience
/// layer over both, not a replacement for either.
/// </para>
/// <para>
/// <b>What this still leaves to the game.</b> A title, a way to type a room code back
/// in, and which prefab is the player are all still the game's to build and name - a
/// state machine cannot draw a screen or decide what the player looks like. What this
/// removes is the part that is easy to get wrong rather than merely tedious: the branch
/// between minting a code and reading one back, the exact frame
/// <see cref="Net.ReplicationReady"/> has to go false (before the join call, never
/// after - see its own remarks for why a client's join burst is built into the menu and
/// destroyed a frame later otherwise) and the exact moment it is safe to leave it alone
/// again (once <see cref="NetSessionDirector.OnAttach"/> has run in the arena - this
/// class deliberately never sets it back true on success, only on cancel or failure),
/// polling <see cref="Net.TraversalState"/> for a line worth showing, a bounded wait
/// before giving up on a join nobody answered, and putting every flag back the way it
/// was found on cancel or failure so a second attempt is not corrupted by the first
/// one's leftovers.
/// </para>
/// <para>
/// <b>Failure modes, named.</b> A join that times out or is refused leaves
/// <see cref="StatusText"/> holding why and returns <see cref="Net.ReplicationReady"/>
/// to true, exactly as if the attempt had never been made - the player sees the menu
/// they left, with a reason, and can try again. A join that succeeds right as this
/// entity is torn down by some other scene change never gets to react; that is no
/// different from any other script losing its last frame to a scene switch, and is not
/// specific to networking.
/// </para>
/// </remarks>
public class NetConnectMenu : EntityScript
{
    // How this reaches the engine. See IEngineBackend: the shipped value is always the
    // direct-P/Invoke backend, and it is a seam only so this can be ticked in a test
    // with no engine behind it.
    private static IEngineBackend Api => EngineBackend.Api;

    /// <summary>
    /// Scene loaded once hosting is ready to start or a join is genuinely connected.
    /// Empty means "stay here" - useful for a game driving the transition itself from
    /// <see cref="EnterArena"/>'s caller.
    /// </summary>
    public string ArenaScene = string.Empty;

    /// <summary>How long a join is given before this gives up on it and says so. The
    /// same reasoning as <see cref="NetSessionDirector.ConnectTimeoutSeconds"/>: without
    /// a bound, a mistyped or dead code leaves the menu waiting forever with no way to
    /// tell the player anything went wrong.</summary>
    public float JoinTimeoutSeconds = 10.0f;

    /// <summary>True from a successful <see cref="Host"/> until <see cref="EnterArena"/>
    /// runs.</summary>
    public bool IsHosting { get; private set; }

    /// <summary>True from a successful <see cref="Join"/> until it connects, fails, times
    /// out, or is cancelled.</summary>
    public bool IsJoining { get; private set; }

    /// <summary>True while either <see cref="IsHosting"/> or <see cref="IsJoining"/> - a
    /// second attempt is refused while one is already in flight rather than silently
    /// abandoning the first.</summary>
    public bool IsBusy => IsHosting || IsJoining;

    /// <summary>The code this peer minted or was handed by <see cref="Host"/>, for the
    /// menu to display. Empty until a host attempt has started.</summary>
    public string HostedRoomCode { get; private set; } = string.Empty;

    /// <summary>One line of human-readable progress or failure, for the menu to show
    /// however it likes - this makes no assumption about where on screen, or in what
    /// font, that belongs.</summary>
    public string StatusText { get; private set; } = string.Empty;

    // Elapsed time on the current join attempt, for JoinTimeoutSeconds.
    private float _joinElapsed;

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (IsHosting)
        {
            if (WantsToEnterArena())
            {
                EnterArena();
            }
            return;
        }
        if (IsJoining)
        {
            TickJoining(deltaTime);
        }
    }

    /// <summary>Whether the player wants to leave the lobby and enter
    /// <see cref="ArenaScene"/> now that hosting is ready. Enter by default - the room
    /// code is on screen and the player presses on when they are done reading it.
    /// Override to gate this on a button instead; return false always and call
    /// <see cref="EnterArena"/> from that button's handler.</summary>
    protected virtual bool WantsToEnterArena() => Api.InputIsKeyPressed(Key.Enter);

    /// <summary>Whether the player wants to give up on the join in progress. Escape by
    /// default, mirroring <see cref="NetSessionDirector.WantsToLeave"/>.</summary>
    protected virtual bool WantsToCancelJoin() => Api.InputIsKeyPressed(Key.Escape);

    /// <summary>
    /// Host under a fresh or supplied room code and remember it for display. Never
    /// touches <see cref="Net.ReplicationReady"/> - a host originates the join burst
    /// rather than receiving one, so there is nothing for it to be refused.
    /// </summary>
    /// <param name="code">A code to host under, e.g. one a player was handed by
    /// somebody re-hosting after a drop. Null or empty mints a fresh one.</param>
    /// <param name="port">Local UDP port to bind. 0 lets the OS choose - see
    /// <see cref="NetSession.BeginHostWithCode"/> for why there is normally no reason to
    /// pin it.</param>
    /// <param name="maxConnections">How many CLIENTS to accept.</param>
    /// <returns>False if already busy or the local bind failed - see
    /// <see cref="Net.LastError"/> and <see cref="StatusText"/>.</returns>
    public bool Host(string? code = null, int port = 0, int maxConnections = 32)
    {
        if (IsBusy)
        {
            return false;
        }
        (bool started, string roomCode) = NetSession.BeginHostWithCode(code, port, maxConnections);
        if (!started)
        {
            StatusText = $"Could not host - {Api.NetLastError}";
            return false;
        }
        HostedRoomCode = roomCode;
        IsHosting = true;
        StatusText = $"Hosting {roomCode}. Press Enter to start.";
        return true;
    }

    /// <summary>
    /// Join the session published under <paramref name="code"/>. Sets
    /// <see cref="Net.ReplicationReady"/> false before the attempt starts - this peer is
    /// standing on the menu, not the arena - and <see cref="TickJoining"/> is what
    /// carries the attempt to a connection, a failure, or a timeout from here.
    /// </summary>
    /// <returns>False if already busy, the code is blank, or the local bind failed. A
    /// true return means traversal has STARTED, not that it will succeed - watch
    /// <see cref="StatusText"/> or <see cref="IsJoining"/> going false for the
    /// outcome.</returns>
    public bool Join(string code)
    {
        if (IsBusy)
        {
            return false;
        }
        string trimmed = code is null ? string.Empty : code.Trim();
        if (trimmed.Length == 0)
        {
            return false;
        }
        // Before the call, not after: BeginJoinByCode can succeed and hand traffic to
        // the transport on the same frame, and a burst that arrives before this line
        // runs would be built into whatever scene this peer is standing on right now.
        Api.NetSetReplicationReady(false);
        if (!NetSession.BeginJoinByCode(trimmed))
        {
            Api.NetSetReplicationReady(true);
            StatusText = $"Could not join '{trimmed}' - {Api.NetLastError}";
            return false;
        }
        IsJoining = true;
        _joinElapsed = 0.0f;
        StatusText = $"Joining {trimmed}...";
        return true;
    }

    /// <summary>Give up on the join in progress and put every flag it touched back the
    /// way <see cref="Join"/> found them, so a second attempt starts clean.</summary>
    /// <param name="reason">Left in <see cref="StatusText"/> for the menu to show.</param>
    public void CancelJoin(string reason = "Cancelled.")
    {
        if (!IsJoining)
        {
            return;
        }
        Api.NetDisconnect();
        Api.NetSetReplicationReady(true);
        IsJoining = false;
        StatusText = reason;
    }

    /// <summary>
    /// Leave the menu for <see cref="ArenaScene"/>. Called automatically once hosting is
    /// confirmed and <see cref="WantsToEnterArena"/> says go, and once a join genuinely
    /// connects; exposed publicly for a game whose own UI wants to drive the moment
    /// instead (a "Start" button rather than a key press).
    /// </summary>
    /// <remarks>
    /// Deliberately never touches <see cref="Net.ReplicationReady"/>. On the host path it
    /// was never set false. On the join path it stays false straight through this call -
    /// <see cref="NetSessionDirector.OnAttach"/> is what sets it true, the moment this
    /// peer is actually standing in the scene the session's entities belong to, which is
    /// the entire property this class exists to get right.
    /// </remarks>
    public void EnterArena()
    {
        IsHosting = false;
        if (ArenaScene.Length > 0)
        {
            Api.SceneLoad(ArenaScene);
        }
    }

    /// <summary>
    /// Skip the session entirely and go straight to <see cref="ArenaScene"/>. Offline,
    /// <see cref="NetSessionDirector"/> spawns one local player there exactly as it does
    /// when that scene is opened directly in the editor - this is the in-game equivalent
    /// of that same fallback, for a menu that wants to offer "play solo" as a button
    /// rather than requiring the editor.
    /// </summary>
    public void PlaySolo()
    {
        if (IsBusy)
        {
            return;
        }
        if (ArenaScene.Length > 0)
        {
            Api.SceneLoad(ArenaScene);
        }
    }

    /// <summary>One frame of a join attempt: cancel, judge, narrate, repeat.</summary>
    private void TickJoining(float deltaTime)
    {
        if (WantsToCancelJoin())
        {
            CancelJoin("Cancelled.");
            return;
        }

        _joinElapsed += deltaTime;
        NetTraversalState state = Api.NetTraversalState;
        if (state == NetTraversalState.Failed)
        {
            string why = Api.NetTraversalError;
            CancelJoin(why.Length > 0 ? why : "Could not join - nothing answered");
            return;
        }
        if (Api.NetIsConnected)
        {
            // Success. IsJoining goes false here, not inside EnterArena, so a game that
            // overrides EnterArena still sees a consistent IsJoining/IsHosting pair the
            // moment it runs.
            IsJoining = false;
            EnterArena();
            return;
        }
        if (_joinElapsed >= JoinTimeoutSeconds)
        {
            CancelJoin("Timed out waiting for the host.");
            return;
        }
        StatusText = $"Joining: {DescribeTraversal(state)} ({_joinElapsed:0.0}s)";
    }

    /// <summary>One line of status text per <see cref="NetTraversalState"/> rung,
    /// mirroring <see cref="NetSessionDirector"/>'s own reconnect narration so a game
    /// using both reads consistent language for the same underlying ladder.</summary>
    private static string DescribeTraversal(NetTraversalState state) => state switch
    {
        NetTraversalState.Mapping => "asking your router for a path in",
        NetTraversalState.Signaling => "finding your friend",
        NetTraversalState.Punching => "opening a path",
        NetTraversalState.Relaying => "relaying through a server",
        _ => "connecting",
    };
}
