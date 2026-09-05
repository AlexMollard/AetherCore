using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen: choose a name, then host a session under a room code or join one by
/// typing a code back in. Remembers the name and the last few directly-typed addresses
/// between runs, and is where a session that ended badly reports why.
/// </summary>
/// <remarks>
/// <para>
/// Opening the session goes through <see cref="NetSession"/> rather than <see cref="Net"/>
/// directly. Starting a session and recording what kind of session it is are one decision:
/// a join that forgets its address or code has no way back after a dropped link, and a
/// host that leaves a stale one behind will try to reconnect to somebody else's. Keeping
/// the pair inside the SDK is what stops the arena and this screen drifting apart.
/// </para>
/// <para>
/// <b>Nobody forwards a port to play this.</b> Hosting mints a 6-character room code
/// (<see cref="NetSession.BeginHostWithCode"/>) and opens it through the engine's own
/// connect ladder - a router asked nicely first, then a STUN hole punch, then a TURN
/// relay if one is configured - rather than binding a fixed port a router has to be told
/// about by hand. Joining takes that code back, not an address. A typed IP[:port] is kept
/// as an explicit fallback: genuinely useful on a LAN or against a known address, and
/// reached by typing one instead of a code - see <see cref="LooksLikeRoomCode"/>.
/// </para>
/// <para>
/// <b>This screen waits out the connection itself</b>, which is where a player expects to
/// see it. A join is started with <see cref="Net.ReplicationReady"/> set false - this peer
/// is standing on a menu, not in the arena, and a replicated entity is created into
/// whichever scene it happens to be in - so the host's join burst is refused rather than
/// built into the title screen and destroyed a frame later. The arena is entered only once
/// the session is genuinely live, and <see cref="NetSessionDirector"/> declares this peer
/// ready as it attaches there, which is what makes the host resend the world. Hosting
/// waits for one explicit press instead: the code is the only way in, so the host gets a
/// moment to read it out before the title screen goes away and takes it with it.
/// </para>
/// <para>
/// <b>What still needs someone else's infrastructure.</b> Two players on the same network
/// need nothing at all - LAN discovery is the connect ladder's own zero-configuration
/// default. Two players on different networks need a rendezvous server: either the game
/// itself ships one, by setting <c>network.rendezvousHost</c> in
/// <c>ProjectSettings.toml</c> (see <c>tools/rendezvous/</c> and
/// <c>docs/multiplayer.md</c>) so every player gets it for free with nothing to type, or a
/// player types one themselves into <see cref="RendezvousField"/>
/// (<see cref="WhisperPrefs.RendezvousAddress"/>, remembered like the player's name) for a
/// specific server that overrides the project's own. Either way it only ever carries the
/// tiny candidate blobs that let two NATs find each other, never game traffic - see
/// <see cref="ConfigureSignaling"/> for exactly how the two are reconciled. A NAT that a
/// hole punch cannot get through past that needs a TURN relay, which is off by default and
/// configured outside this screen; when the ladder fails for that reason,
/// <see cref="Net.RelayConfigured"/> says so and this screen reports it plainly rather than
/// leaving a spinner running.
/// </para>
/// <para>
/// <b>Layout</b> (the numbers live in <c>Title.scene.toml</c>, the reasoning has to live
/// somewhere the editor will not overwrite on its next save). The reference canvas is
/// 1920x1080 and UI anchor Y=0 is the TOP. Everything hangs off one left margin at x 0.094
/// and two hairlines - a header band at y 0.072 and a footer band at y 0.885 - so the page
/// has edges without needing a border. Nothing is centred: a centred form has no reading
/// order, and this screen is a form. The two cards are the two questions it asks (who are
/// you, where are you going), each wearing a 3px accent bar on its top edge, the same motif
/// as the stub under the wordmark. The status line sits between the cards and the footer
/// rule with a coloured disc beside it, so the screen's state is legible before a word of it
/// is read. All text is ASCII: the font pipeline bakes nothing else.
/// </para>
/// </remarks>
public sealed class ConnectScreen : EntityScript
{
    private static readonly Vector4 Accent = new(0.545f, 0.729f, 0.949f, 1.0f);
    private static readonly Vector4 Muted = new(0.478f, 0.518f, 0.596f, 1.0f);
    private static readonly Vector4 DotIdle = new(0.294f, 0.320f, 0.384f, 1.0f);
    private static readonly Vector4 Bad = new(1.000f, 0.451f, 0.396f, 1.0f);

    // Crockford room codes are always exactly this many characters once the spaces and
    // dashes a player types for readability are stripped - see aether::net::kRoomCodeLength
    // and RoomCode.cpp's NormalizeRoomCode. That fixed length, not a character-set guess,
    // is what tells a typed address (which is never exactly six symbols once you drop its
    // dots and colons - or, unpunctuated, is a hostname of some other length) apart from a
    // code, with no separate "connect by IP instead" control needed at all.
    private const int RoomCodeLength = 6;

    /// <summary>The player's display name. No <c>UiTextBoxRef</c> component-ref wrapper
    /// exists in this build (see <c>ComponentRef.cs</c>), so this is a plain entity slot
    /// read through <see cref="Ui.GetTextBoxText"/>.</summary>
    public Entity NameField;

    /// <summary>
    /// What to join: a room code by default, or a typed <c>host[:port]</c> as the
    /// explicit fallback - see <see cref="LooksLikeRoomCode"/> for how this screen tells
    /// the two apart. Same plain-entity route as <see cref="NameField"/>.
    /// </summary>
    public Entity JoinField;

    /// <summary>Rendezvous server (<c>host:port</c>) to exchange candidates through for a
    /// join across two different networks - an override for a specific server, checked
    /// before this project's own <c>network.rendezvousHost</c> default (see
    /// <see cref="ConfigureSignaling"/>). Blank defers to that project default, or to
    /// LAN-only if it too is unset - see <see cref="WhisperPrefs.RendezvousAddress"/>.
    /// Optional: an invalid entity here just means a player cannot type their own override.</summary>
    public Entity RendezvousField;

    /// <summary>Starts hosting under a fresh room code.</summary>
    public Entity HostButton;

    /// <summary>Connects to whatever <see cref="JoinField"/> holds.</summary>
    public Entity JoinButton;

    /// <summary>Shows the room code once hosting has started - the entire join
    /// credential, so it stays on screen (and on the clipboard) until the host chooses to
    /// leave for the arena. Optional: an invalid entity here just means the code is only
    /// ever on the clipboard, not on screen.</summary>
    public Entity RoomCodeText;

    /// <summary>Where progress and errors are reported.</summary>
    public Entity StatusText;

    /// <summary>The small disc beside <see cref="StatusText"/>. Carries the state as colour
    /// so the screen reads at a glance without anybody parsing a sentence.</summary>
    public Entity StatusDot;

    /// <summary>Port assumed when a typed address omits one, and when hosting binds with
    /// no port pinned. Only the direct-IP fallback cares about a specific number - a
    /// room-code host lets the OS choose, since whatever a joiner actually reaches is
    /// whatever the router maps or the punch discovers, never this value.</summary>
    public ushort DefaultPort = 7777;

    /// <summary>How long a join is given to be answered before this screen gives up on it.
    /// A mistyped address, or a code with nobody behind it, is answered by nobody at all,
    /// so something has to be counting or the player waits on a spinner for as long as
    /// they are willing to.</summary>
    public float JoinTimeoutSeconds = 8.0f;

    // The recent-server rows, found by name rather than wired one by one: they are an
    // indexed set of identical elements, and four more entity properties on the scene entity
    // would say nothing the names do not. Direct addresses only - a room code is one-shot
    // and useless to re-offer once its host has moved on, so a successful code join is
    // never remembered here.
    private readonly Entity[] _recentRows = new Entity[WhisperPrefs.RecentCapacity];
    private Entity _recentEmpty;

    private bool _nameWasEditing;
    private bool _rendezvousWasEditing;
    private bool _focusSeeded;
    private bool _leaving;

    // Set once BeginHostWithCode succeeds; cleared only by leaving for the arena. While
    // true, HostButton no longer starts a second hosting attempt - it reads "ENTER ARENA"
    // and this screen is just holding the code on screen for the host to read out.
    private bool _hostPending;
    private string _roomCode = string.Empty;

    // The join in flight, and how long it has been in flight for. `_joinTarget` doubles as
    // "a join is being waited on", so there is one thing to test rather than two that can
    // disagree. `_joinIsCode` picks which of the two very differently-shaped wait loops
    // below is watching it.
    private string _joinTarget = string.Empty;
    private bool _joinIsCode;
    private float _joinElapsed;

    // The highest rung a code-based attempt reached before failing, tracked so a punch
    // failure can be told apart from a mapping or signalling failure - see DescribeFailure.
    private NetTraversalState _peakTraversalState = NetTraversalState.Idle;

    // Whether THIS process has ever made an explicit signalling choice (rendezvous
    // or LAN) via ConfigureSignaling below. Static, not per-instance, for the same
    // reason WhisperPrefs and NetSession.HostRoomCode are: NetworkContext's own
    // signalling choice (NetTraversalSession::m_signalingChosen) lives for the whole
    // process too, and outlives this screen across an Arena round trip - an
    // instance field here would forget an earlier explicit choice on the very scene
    // reload that most needs to remember it (see ConfigureSignaling's remarks).
    // Known ceiling: repeated Play/Stop of the SAME editor process can leave this
    // true while a freshly-constructed NetworkContext's own flag is false again,
    // which very briefly hides the engine's own rendezvous default on the next Play
    // - not a concern for a published build, which never does that.
    private static bool s_signalingExplicit;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        WhisperPrefs.EnsureLoaded();
        for (int i = 0; i < _recentRows.Length; i++)
        {
            _recentRows[i] = Scene.Find($"Recent{i}");
        }
        _recentEmpty = Scene.Find("RecentEmpty");

        // Refused at the keystroke rather than clipped at commit - the same
        // arrangement the chat box uses for messages. Clean still enforces it (and
        // the character filter) on the way in: a length the box honoured is not a
        // length a paste or a script has to. Set before the saved name goes in, so
        // an over-long value from a hand-edited file is trimmed once, visibly.
        Ui.SetMaxLength(NameField, WhisperPrefs.MaxNameLength);
        Ui.SetTextBoxText(NameField, WhisperPrefs.PlayerName);
        if (RendezvousField.IsValid)
        {
            Ui.SetMaxLength(RendezvousField, WhisperPrefs.MaxNameLength);
            Ui.SetTextBoxText(RendezvousField, WhisperPrefs.RendezvousAddress);
        }
        RefreshRecentList();

        // Explain an involuntary return - the arena sends us back here when a connection
        // fails or the host goes away - because an unexplained title screen looks like a
        // crash. Taken rather than read so a later voluntary visit is not still apologising
        // for it.
        string carried = NetSession.TakeStatusMessage();
        SetStatus(carried, carried.Length > 0 ? Bad : Muted);
    }

    /// <inheritdoc/>
    /// <remarks>
    /// Focus is process-wide, not per scene, and a selectable that keeps it is still
    /// activated by Enter <em>and Space</em> - which is how a menu field quietly turns a
    /// platformer's jump button into "start typing". This screen takes the keyboard
    /// deliberately, so it hands it back deliberately too, here and again immediately before
    /// the scene change.
    /// </remarks>
    public override void OnDetach() => Ui.ClearFocus();

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (_leaving)
        {
            return; // the scene change is already requested; ignore anything else pressed
        }

        if (_joinTarget.Length > 0)
        {
            TickJoin(deltaTime);
            return; // one attempt at a time: the screen is a progress report until it ends
        }

        if (_hostPending)
        {
            TickHostPending();
            return; // the code is up; nothing left to do here but wait for ENTER ARENA
        }

        SeedFocus();
        CaptureNameOnEdit();
        CaptureRendezvousOnEdit();

        if (Ui.WasSubmitted(NameField))
        {
            // Committing the name moves the player on rather than leaving them in a field
            // they have finished with.
            CommitName();
            Ui.SetFocus(JoinField);
            return;
        }

        if (Ui.WasActivated(HostButton))
        {
            StartHost();
            return;
        }

        if (Ui.WasActivated(JoinButton) || Ui.WasSubmitted(JoinField))
        {
            StartJoin(Ui.GetTextBoxText(JoinField));
            return;
        }

        for (int i = 0; i < _recentRows.Length; i++)
        {
            if (_recentRows[i].IsValid && Ui.WasActivated(_recentRows[i]))
            {
                // Show the choice in the field as well as acting on it, so the address the
                // attempt is using is the one the player can see and edit on a retry. These
                // rows only ever hold addresses (see the field above), so there is no need
                // to run the room-code heuristic on a value that was never a code.
                string address = Ui.GetButtonLabel(_recentRows[i]);
                Ui.SetTextBoxText(JoinField, address);
                StartJoinAddress(address);
                return;
            }
        }
    }

    // ── Starting ────────────────────────────────────────────────────────────────

    /// <summary>Open a session as the host, under a freshly minted room code.</summary>
    /// <remarks>
    /// The cap passed to <see cref="NetSession.BeginHostWithCode"/> counts CONNECTIONS,
    /// and a host is not one of its own, so a four-player game hosts with three - see
    /// <see cref="WhisperSession.MaxPlayers"/>. Port 0 lets the OS choose one to bind: the
    /// endpoint a joiner actually reaches is whatever the router maps or the punch
    /// discovers, never that number, so there is nothing here to configure a router with.
    /// </remarks>
    private void StartHost()
    {
        CommitName();
        ConfigureSignaling();
        (bool started, string code) = NetSession.BeginHostWithCode(port: 0, maxConnections: WhisperSession.MaxPlayers - 1);
        if (!started)
        {
            Fail($"Could not host - {DescribeFailure()}");
            return;
        }

        // Copied the moment it exists: this code is the entire join credential, and a
        // player reading it off screen rather than off the clipboard is the only one who
        // has to get every character right by eye.
        _roomCode = code;
        _hostPending = true;
        Input.Clipboard = code;
        if (RoomCodeText.IsValid)
        {
            Ui.SetText(RoomCodeText, $"YOUR CODE: {code}  (copied to clipboard)");
        }
        Ui.SetButtonLabel(HostButton, "ENTER ARENA");
        Ui.SetFocus(HostButton);
        SetStatus($"Room {code} is open. Read it out, then press ENTER ARENA whenever you're ready.", Accent);
    }

    /// <summary>Watches for the explicit press that leaves <see cref="StartHost"/>'s
    /// waiting room. Hosting itself never blocks - a listening socket accepting the
    /// connect ladder is live the moment <see cref="StartHost"/> returns - this is purely
    /// the courtesy of not sweeping the code off screen before the host has read it.</summary>
    private void TickHostPending()
    {
        if (Ui.WasActivated(HostButton))
        {
            Enter($"Hosting room {_roomCode}.");
        }
    }

    /// <summary>Begin connecting: a room code by default, or the typed address fallback -
    /// see <see cref="LooksLikeRoomCode"/> for how this screen tells them apart.</summary>
    private void StartJoin(string typed)
    {
        CommitName();
        string trimmed = (typed ?? string.Empty).Trim();
        if (LooksLikeRoomCode(trimmed))
        {
            StartJoinByCode(trimmed);
        }
        else
        {
            StartJoinAddress(trimmed);
        }
    }

    /// <summary>
    /// True when <paramref name="typed"/> is shaped like a room code rather than an
    /// address: exactly <see cref="RoomCodeLength"/> symbols once the spaces and dashes a
    /// player types for readability are dropped, and containing neither a dot nor a colon
    /// (which only ever appear in an address). This is the whole "explicit fallback" -
    /// there is no separate mode switch, because a fixed-length code and a punctuated
    /// address never look alike, and a bare unpunctuated hostname of some other length
    /// (<c>localhost</c>, nine symbols) still reads as an address rather than a
    /// malformed code.
    /// </summary>
    private static bool LooksLikeRoomCode(string typed)
    {
        int symbols = 0;
        foreach (char c in typed)
        {
            if (c == ' ' || c == '-')
            {
                continue;
            }
            if (c == '.' || c == ':')
            {
                return false;
            }
            symbols++;
        }
        return symbols == RoomCodeLength;
    }

    /// <summary>Begin joining a room code over the connect ladder - no address, no
    /// forwarded port, on either side.</summary>
    /// <remarks>
    /// <see cref="Net.ReplicationReady"/> goes false FIRST, before the join and before
    /// anything can arrive - see <see cref="StartJoinAddress"/>'s remarks, which apply
    /// identically here.
    /// </remarks>
    private void StartJoinByCode(string code)
    {
        ConfigureSignaling();
        Net.ReplicationReady = false;
        if (!NetSession.BeginJoinByCode(code))
        {
            Net.ReplicationReady = true;
            Fail($"'{code}' - {DescribeFailure()}");
            return;
        }

        _joinTarget = code;
        _joinIsCode = true;
        _peakTraversalState = NetTraversalState.Idle;
        _joinElapsed = 0.0f;
        Ui.ClearFocus(); // the screen is a progress report now; nothing here is pressable
        SetStatus($"Room {code}: {DescribeTraversal(Net.TraversalState)}", Accent);
    }

    /// <summary>Begin connecting to a typed address - the explicit fallback, kept for a
    /// LAN or a known address, and wait for the answer here.</summary>
    /// <remarks>
    /// <see cref="Net.ReplicationReady"/> goes false FIRST, before the connect and before
    /// anything can arrive: this peer is standing on a menu, and a replicated entity is
    /// built into whichever scene the peer is in when its spawn lands. Told to hold, the
    /// client refuses the host's join burst instead of building the whole session's cast
    /// into the title screen and destroying it on the scene change.
    /// </remarks>
    private void StartJoinAddress(string typed)
    {
        (string ip, ushort port) = NetSession.ParseAddress(typed, DefaultPort);
        string target = $"{ip}:{port}";
        Net.ReplicationReady = false;
        if (!NetSession.BeginJoin(ip, port))
        {
            // The socket would not open, or the address would not resolve. Nothing was
            // started, so there is nothing to tear down - just say so and stay put.
            Net.ReplicationReady = true;
            Fail($"Could not reach {target} - {Describe(Net.LastError)}");
            return;
        }

        // Recorded on the ATTEMPT, not on success, so an address that turns out to be
        // wrong is still in the list to be corrected. The list is a short MRU, so a
        // mistyped one is pushed off the end by the next few real ones.
        WhisperPrefs.Remember(target);
        WhisperPrefs.Save();
        _joinTarget = target;
        _joinIsCode = false;
        _joinElapsed = 0.0f;
        Ui.ClearFocus(); // the screen is a progress report now; nothing here is pressable
        SetStatus($"Connecting to {target}...", Accent);
    }

    /// <summary>Watch whichever attempt <see cref="StartJoin"/> started, and enter the
    /// arena only once the session is genuinely live.</summary>
    private void TickJoin(float deltaTime)
    {
        _joinElapsed += deltaTime;
        if (_joinIsCode)
        {
            TickJoinByCode();
        }
        else
        {
            TickJoinAddress();
        }
    }

    /// <summary>
    /// <see cref="TickJoin"/> for a room code. The transport itself is misleading for as
    /// long as the connect ladder is punching - a joiner binds a socket exactly like a
    /// host does, so <see cref="Net.IsClient"/> stays false the whole time the ladder
    /// works and only flips once the real handshake finishes - so progress and failure
    /// are read off <see cref="Net.TraversalState"/> instead, the same signal
    /// <see cref="NetSessionDirector"/> uses for a code-based reconnect.
    /// </summary>
    private void TickJoinByCode()
    {
        NetTraversalState state = Net.TraversalState;
        if (state != NetTraversalState.Failed && state > _peakTraversalState)
        {
            _peakTraversalState = state;
        }

        if (state == NetTraversalState.Failed)
        {
            AbandonJoin($"Room {_joinTarget}: {DescribeFailure()}");
            return;
        }

        if (Net.IsConnected)
        {
            Enter($"Connected to room {_joinTarget}.");
            return;
        }

        if (_joinElapsed >= JoinTimeoutSeconds)
        {
            AbandonJoin($"Room {_joinTarget}: {DescribeNoAnswer()}");
            return;
        }

        SetStatus($"Room {_joinTarget}: {DescribeTraversal(state)} ({_joinElapsed:0.0}s)", Accent);
    }

    /// <summary>
    /// <see cref="TickJoin"/> for a typed address. Three ways it can end. It is
    /// ANSWERED - <see cref="Net.IsConnected"/> goes true once the host has assigned this
    /// peer a connection id - and the arena is entered, where
    /// <see cref="NetSessionDirector"/> declares this peer ready and the host resends the
    /// world into the scene it is now actually standing in. It is REFUSED, which arrives
    /// with a reason to show (a full server, or a host shutting down). Or nobody answers at
    /// all: the transport gives up on its own after a few seconds, which drops the role
    /// back to offline, and <see cref="JoinTimeoutSeconds"/> catches the rest.
    /// </summary>
    private void TickJoinAddress()
    {
        string reason = Net.DisconnectReason;
        if (reason.Length > 0)
        {
            AbandonJoin($"{_joinTarget} refused the connection - {reason}");
            return;
        }

        if (Net.IsConnected)
        {
            Enter($"Connected to {_joinTarget}.");
            return;
        }

        if (!Net.IsClient)
        {
            AbandonJoin($"Could not reach {_joinTarget} - nothing is listening there");
            return;
        }

        if (_joinElapsed >= JoinTimeoutSeconds)
        {
            AbandonJoin($"No answer from {_joinTarget} after {JoinTimeoutSeconds:0} seconds");
            return;
        }

        SetStatus($"Connecting to {_joinTarget}... {_joinElapsed:0.0}s", Accent);
    }

    /// <summary>Give up on the attempt in flight and hand the screen back to the player.</summary>
    /// <remarks>
    /// <see cref="Net.Disconnect"/> even when the link is already gone: it is what clears a
    /// half-open attempt - a bound traversal socket as much as a live connection - out of
    /// the way of the next one, and it is safe with no session. Readiness goes back to its
    /// default with it - the hold belonged to this attempt.
    /// </remarks>
    private void AbandonJoin(string message)
    {
        Net.Disconnect();
        Net.ReplicationReady = true;
        NetSession.JoinRequested = false;
        _joinTarget = string.Empty;
        _joinIsCode = false;
        _joinElapsed = 0.0f;
        _focusSeeded = false; // the screen is interactive again, so give it the keyboard back
        Fail(message);
    }

    /// <summary>Leave for the arena, saying what was started on the way out.</summary>
    private void Enter(string message)
    {
        _leaving = true;
        SetStatus(message, Accent);
        // Hand the keyboard back before the arena exists to take it.
        Ui.ClearFocus();
        Scene.Load("Arena");
    }

    /// <summary>Report why nothing started and leave the screen exactly as it was.</summary>
    private void Fail(string message)
    {
        SetStatus(message, Bad);
        Log.Warn($"[Whisper] {message}");
    }

    // ── Signalling ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Choose how candidates are exchanged, before every <see cref="StartHost"/> or
    /// <see cref="StartJoinByCode"/>: through whatever address the player typed into
    /// <see cref="RendezvousField"/>, else this project's own
    /// <c>network.rendezvousHost</c> default if one is configured, else the local
    /// network with no server at all.
    /// </summary>
    /// <remarks>
    /// Neither <see cref="NetSession.BeginHostWithCode"/> nor
    /// <see cref="NetSession.BeginJoinByCode"/> makes this choice itself -
    /// deliberately, so a project's own default is never silently overridden by a
    /// menu that does not know it exists. This method mirrors that: a blank field
    /// leaves the backend UNCONFIGURED rather than forcing LAN, so
    /// <c>NetTraversalSession::SetRendezvousDefault</c> gets its one chance to apply
    /// the project's setting - UNLESS this screen already made an explicit choice
    /// earlier in this process (<see cref="s_signalingExplicit"/>), in which case
    /// silence here would leave that earlier choice's address stale rather than
    /// genuinely reverting to LAN-only, which is what clearing the field means.
    /// </remarks>
    private void ConfigureSignaling()
    {
        string rendezvous = WhisperPrefs.RendezvousAddress;
        if (rendezvous.Length > 0)
        {
            Net.UseRendezvousSignaling(rendezvous);
            s_signalingExplicit = true;
            return;
        }
        if (s_signalingExplicit)
        {
            // This screen chose Rendezvous at some earlier point in this process and
            // the field has since been cleared - that is itself a deliberate choice
            // ("go back to LAN-only"), not silence, so it must be forced explicitly
            // or the stale address stays in effect (see the class remarks above on
            // why this method exists at all).
            Net.UseLanSignaling();
            return;
        }
        // Neither this screen nor an earlier attempt this process has chosen
        // anything: leave it unconfigured so EngineSettings.Network.rendezvousHost -
        // set once in ProjectSettings.toml, needing no code and no per-player typing
        // at all - gets its one chance to apply (NetTraversalSession::
        // SetRendezvousDefault re-reads it on every Host/Join and only ever fills a
        // gap, never overrides). Calling UseLanSignaling here, as this used to do
        // unconditionally, would make that setting permanently unreachable from the
        // very first title-screen visit of every run.
    }

    /// <summary>One line of status text per <see cref="NetTraversalState"/> rung, matching
    /// the wording <see cref="NetSessionDirector"/> shows for a code-based reconnect so the
    /// vocabulary a player learns here still means the same thing later.</summary>
    private static string DescribeTraversal(NetTraversalState state) => state switch
    {
        NetTraversalState.Mapping => "asking your router for a path in",
        NetTraversalState.Signaling => "finding your friend",
        NetTraversalState.Punching => "opening a path",
        NetTraversalState.Relaying => "relaying through a server",
        _ => "connecting",
    };

    /// <summary>Why a code-based attempt failed, naming a missing relay specifically when
    /// that is plausibly what was missing.</summary>
    /// <remarks>
    /// The punch failing and no relay being configured are two separate facts - see
    /// <see cref="Net.RelayConfigured"/> - and only their CONJUNCTION is worth calling out:
    /// a mapping or signalling failure has nothing to do with a relay, and a relay that was
    /// tried and still failed already says so in <see cref="Net.TraversalError"/> without
    /// this screen guessing at it. <c>_peakTraversalState</c> is what tells "reached
    /// Punching before failing" apart from "never got that far".
    /// </remarks>
    private string DescribeFailure()
    {
        string reason = Describe(Net.TraversalError);
        if (_peakTraversalState >= NetTraversalState.Punching && !Net.RelayConfigured)
        {
            reason += " - this network needs a relay and none is configured";
        }
        return reason;
    }

    /// <summary>Why a code-based join timed out with nobody ever answering - as
    /// opposed to <see cref="DescribeFailure"/>'s native Failed states, this rung has
    /// no error to read: LAN broadcast reaching nobody on another network and a code
    /// with nobody behind it look identical from here, both "asked, nothing came
    /// back". Named specifically only when <c>_peakTraversalState</c> never even
    /// reached <see cref="NetTraversalState.Punching"/> (a peer WAS heard from past
    /// that point, so this is not why it stalled) and this screen's own rendezvous
    /// field is blank - the one half of "is a rendezvous server configured" this
    /// screen can actually see; ProjectSettings.toml's own network.rendezvousHost is
    /// applied engine-side and not readable from here, so this names what IT knows
    /// rather than overclaiming about a default it cannot see.</summary>
    private string DescribeNoAnswer()
    {
        string reason = $"no answer after {JoinTimeoutSeconds:0} seconds";
        if (_peakTraversalState < NetTraversalState.Punching && WhisperPrefs.RendezvousAddress.Length == 0)
        {
            reason += " - no rendezvous address is set above, so only a host on this same network can be found; " +
                      "paste one there, or set network.rendezvousHost in ProjectSettings.toml, for internet play";
        }
        return reason;
    }

    // ── Screen state ────────────────────────────────────────────────────────────

    /// <summary>Focus something on entry, once, on the first tick rather than in
    /// <see cref="OnAttach"/> - the navigation system runs before scripts, so an element
    /// focused here is one it has already seen this frame.</summary>
    /// <remarks>
    /// Nothing focuses itself any more, so a screen that wants the keyboard has to say so.
    /// A returning player already has a name and wants HOST; a new one wants the field they
    /// are about to fill in.
    /// </remarks>
    private void SeedFocus()
    {
        if (_focusSeeded)
        {
            return;
        }
        _focusSeeded = true;
        Ui.SetFocus(WhisperPrefs.PlayerName.Length == 0 ? NameField : HostButton);
    }

    /// <summary>Persist the name the moment the player finishes with the field, not only
    /// when they get as far as connecting. A preference that is only saved on success is one
    /// that is lost by anybody who tried and gave up.</summary>
    private void CaptureNameOnEdit()
    {
        bool editing = Ui.IsEditing(NameField);
        if (_nameWasEditing && !editing)
        {
            CommitName();
        }
        _nameWasEditing = editing;
    }

    /// <summary>Take the typed name for this session and for the next launch.</summary>
    /// <remarks>
    /// <see cref="NetSession.LocalPlayerName"/> substitutes "Player" for a blank, which is
    /// what the session should use; the empty string is what is SAVED, so a player who has
    /// not chosen a name still sees the field's placeholder next launch rather than a name
    /// they never picked.
    /// </remarks>
    private void CommitName()
    {
        string raw = Ui.GetTextBoxText(NameField);
        // Cleaned BEFORE it goes live: the name is owner-authored replication read
        // by every peer, and WhisperPrefs.Clean on load alone would only protect
        // against a hand-edited file, not against what is typed this session. This
        // is the front half of ChatBox's own name sanitising - that one catches a
        // modified client; this one catches an honest player with a non-ASCII
        // keyboard layout.
        string typed = WhisperPrefs.Clean(raw);
        if (typed != raw)
        {
            // Show the form that will be used, so the name that connects is the
            // name the player sees.
            Ui.SetTextBoxText(NameField, typed);
        }
        NetSession.LocalPlayerName = typed;
        if (typed == WhisperPrefs.PlayerName)
        {
            return;
        }
        WhisperPrefs.PlayerName = typed;
        WhisperPrefs.Save();
    }

    /// <summary>Persist the rendezvous address the moment the player finishes with the
    /// field - the same on-blur discipline as <see cref="CaptureNameOnEdit"/>, and for the
    /// same reason: a value only saved on a successful connect is lost by the player who
    /// typed it and then had the attempt fail.</summary>
    private void CaptureRendezvousOnEdit()
    {
        if (!RendezvousField.IsValid)
        {
            return;
        }
        bool editing = Ui.IsEditing(RendezvousField);
        if (_rendezvousWasEditing && !editing)
        {
            CommitRendezvous();
        }
        _rendezvousWasEditing = editing;
    }

    private void CommitRendezvous()
    {
        string raw = Ui.GetTextBoxText(RendezvousField);
        string typed = WhisperPrefs.Clean(raw);
        if (typed != raw)
        {
            Ui.SetTextBoxText(RendezvousField, typed);
        }
        if (typed == WhisperPrefs.RendezvousAddress)
        {
            return;
        }
        WhisperPrefs.RendezvousAddress = typed;
        WhisperPrefs.Save();
    }

    /// <summary>Show one row per remembered server, and the placeholder when there are
    /// none. A row with no address is switched off rather than left blank, so keyboard
    /// navigation cannot land on an empty line.</summary>
    private void RefreshRecentList()
    {
        IReadOnlyList<string> recent = WhisperPrefs.Recent;
        for (int i = 0; i < _recentRows.Length; i++)
        {
            if (!_recentRows[i].IsValid)
            {
                continue;
            }
            bool used = i < recent.Count;
            if (used)
            {
                Ui.SetButtonLabel(_recentRows[i], recent[i]);
            }
            _recentRows[i].SetActive(used);
        }
        if (_recentEmpty.IsValid)
        {
            _recentEmpty.SetActive(recent.Count == 0);
        }
    }

    private void SetStatus(string message, Vector4 color)
    {
        Ui.SetText(StatusText, message);
        Ui.SetTextColor(StatusText, color);
        if (StatusDot.IsValid)
        {
            Ui.SetImageColor(StatusDot, message.Length == 0 ? DotIdle : color);
        }
    }

    /// <summary>Never show an empty reason. A transport that failed without a message still
    /// has to be reported as something.</summary>
    private static string Describe(string error)
        => error.Length > 0 ? error : "the transport gave no reason";
}
