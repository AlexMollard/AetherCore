using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen: choose a name, then host a session or join one by address. Remembers the
/// name and the last few servers between runs, and is where a session that ended badly
/// reports why.
/// </summary>
/// <remarks>
/// <para>
/// Opening the session goes through <see cref="NetSession"/> rather than <see cref="Net"/>
/// directly. Starting a session and recording what kind of session it is are one decision:
/// a join that forgets its address has no way back after a dropped link, and a host that
/// leaves a stale address behind will try to reconnect to somebody else's. Keeping the pair
/// inside the SDK is what stops the arena and this screen drifting apart.
/// </para>
/// <para>
/// <b>This screen waits out the connection itself</b>, which is where a player expects to
/// see it. A join is started with <see cref="Net.ReplicationReady"/> set false - this peer
/// is standing on a menu, not in the arena, and a replicated entity is created into
/// whichever scene it happens to be in - so the host's join burst is refused rather than
/// built into the title screen and destroyed a frame later. The arena is entered only once
/// the session is genuinely live, and <see cref="NetSessionDirector"/> declares this peer
/// ready as it attaches there, which is what makes the host resend the world. Hosting has
/// no wait at all: a listening socket is live the moment it opens.
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

    /// <summary>The player's display name. No <c>UiTextBoxRef</c> component-ref wrapper
    /// exists in this build (see <c>ComponentRef.cs</c>), so this is a plain entity slot
    /// read through <see cref="Ui.GetTextBoxText"/>.</summary>
    public Entity NameField;

    /// <summary>The host:port to join. Same plain-entity route as <see cref="NameField"/>.</summary>
    public Entity AddressField;

    /// <summary>Starts hosting on <see cref="DefaultPort"/>.</summary>
    public Entity HostButton;

    /// <summary>Connects to the parsed address.</summary>
    public Entity JoinButton;

    /// <summary>Where progress and errors are reported.</summary>
    public Entity StatusText;

    /// <summary>The small disc beside <see cref="StatusText"/>. Carries the state as colour
    /// so the screen reads at a glance without anybody parsing a sentence.</summary>
    public Entity StatusDot;

    /// <summary>Port used when hosting, and when the address field omits one.</summary>
    public ushort DefaultPort = 7777;

    /// <summary>How long a join is given to be answered before this screen gives up on it.
    /// A mistyped address is answered by nobody at all, so something has to be counting or
    /// the player waits on a spinner for as long as they are willing to.</summary>
    public float JoinTimeoutSeconds = 8.0f;

    // The recent-server rows, found by name rather than wired one by one: they are an
    // indexed set of identical elements, and four more entity properties on the scene entity
    // would say nothing the names do not.
    private readonly Entity[] _recentRows = new Entity[WhisperPrefs.RecentCapacity];
    private Entity _recentEmpty;

    private bool _nameWasEditing;
    private bool _focusSeeded;
    private bool _leaving;

    // The join in flight, and how long it has been in flight for. `_joinTarget` doubles as
    // "a join is being waited on", so there is one thing to test rather than two that can
    // disagree.
    private string _joinTarget = string.Empty;
    private float _joinElapsed;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        WhisperPrefs.EnsureLoaded();
        for (int i = 0; i < _recentRows.Length; i++)
        {
            _recentRows[i] = Scene.Find($"Recent{i}");
        }
        _recentEmpty = Scene.Find("RecentEmpty");

        Ui.SetTextBoxText(NameField, WhisperPrefs.PlayerName);
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

        SeedFocus();
        CaptureNameOnEdit();

        if (Ui.WasSubmitted(NameField))
        {
            // Committing the name moves the player on rather than leaving them in a field
            // they have finished with.
            CommitName();
            Ui.SetFocus(AddressField);
            return;
        }

        if (Ui.WasActivated(HostButton))
        {
            StartHost();
            return;
        }

        if (Ui.WasActivated(JoinButton) || Ui.WasSubmitted(AddressField))
        {
            StartJoin(Ui.GetTextBoxText(AddressField));
            return;
        }

        for (int i = 0; i < _recentRows.Length; i++)
        {
            if (_recentRows[i].IsValid && Ui.WasActivated(_recentRows[i]))
            {
                // Show the choice in the field as well as acting on it, so the address the
                // attempt is using is the one the player can see and edit on a retry.
                string address = Ui.GetButtonLabel(_recentRows[i]);
                Ui.SetTextBoxText(AddressField, address);
                StartJoin(address);
                return;
            }
        }
    }

    // ── Starting ────────────────────────────────────────────────────────────────

    /// <summary>Open a session as the host.</summary>
    /// <remarks>
    /// The cap passed to <see cref="Net.Host"/> counts CONNECTIONS, and a host is not one of
    /// its own, so a four-player game hosts with three. The framework refuses the next joiner
    /// with a reason it can read and show, rather than dropping it - which is why the number
    /// lives here, in the game, and the refusal lives in the framework.
    /// </remarks>
    private void StartHost()
    {
        CommitName();
        if (!NetSession.BeginHost(DefaultPort, WhisperSession.MaxPlayers - 1))
        {
            Fail($"Could not host on port {DefaultPort} - {Describe(Net.LastError)}");
            return;
        }
        Enter($"Hosting on port {DefaultPort}...");
    }

    /// <summary>Begin connecting, and wait for the answer here.</summary>
    /// <remarks>
    /// <see cref="Net.ReplicationReady"/> goes false FIRST, before the connect and before
    /// anything can arrive: this peer is standing on a menu, and a replicated entity is
    /// built into whichever scene the peer is in when its spawn lands. Told to hold, the
    /// client refuses the host's join burst instead of building the whole session's cast
    /// into the title screen and destroying it on the scene change.
    /// </remarks>
    private void StartJoin(string typed)
    {
        CommitName();
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
        _joinElapsed = 0.0f;
        Ui.ClearFocus(); // the screen is a progress report now; nothing here is pressable
        SetStatus($"Connecting to {target}...", Accent);
    }

    /// <summary>Watch the attempt started by <see cref="StartJoin"/>, and enter the arena
    /// only once the session is genuinely live.</summary>
    /// <remarks>
    /// Three ways it can end. It is ANSWERED - <see cref="Net.IsConnected"/> goes true once
    /// the host has assigned this peer a connection id - and the arena is entered, where
    /// <see cref="NetSessionDirector"/> declares this peer ready and the host resends the
    /// world into the scene it is now actually standing in. It is REFUSED, which arrives
    /// with a reason to show (a full server, or a host shutting down). Or nobody answers at
    /// all: the transport gives up on its own after a few seconds, which drops the role
    /// back to offline, and <see cref="JoinTimeoutSeconds"/> catches the rest.
    /// </remarks>
    private void TickJoin(float deltaTime)
    {
        _joinElapsed += deltaTime;

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
    /// half-open attempt out of the way of the next one, and it is safe with no session.
    /// Readiness goes back to its default with it - the hold belonged to this attempt.
    /// </remarks>
    private void AbandonJoin(string message)
    {
        Net.Disconnect();
        Net.ReplicationReady = true;
        NetSession.JoinRequested = false;
        _joinTarget = string.Empty;
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
        string typed = Ui.GetTextBoxText(NameField);
        NetSession.LocalPlayerName = typed;
        if (typed == WhisperPrefs.PlayerName)
        {
            return;
        }
        WhisperPrefs.PlayerName = typed;
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
