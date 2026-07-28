using System.Collections.Generic;
using System.Numerics;
using System.Text;
using AetherCore;

namespace AetherGame;

/// <summary>
/// In-game text chat: a single-line input pinned to the bottom-left corner with a
/// timestamped, scrollable transcript above it. Enter opens the box, Enter again sends,
/// and every peer sees the line attributed to the sender's replicated display name. The
/// whole thing fades out of the way when nothing is happening.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why this script lives on the player prefab.</b> The host only accepts a
/// <see cref="NetRpcTarget.Server"/> call aimed at an entity the SENDING connection
/// OWNS. A chat submission is exactly that kind of call, so <see cref="SendChat"/> has
/// to be declared on a script attached to the caller's own player. Putting it on the
/// scene-placed, host-owned <c>Session</c> entity would compile, route, and then be
/// silently dropped on arrival.
/// </para>
/// <para>
/// That placement means every peer runs one instance of this script per player in the
/// session, not one per process. Two consequences shape the rest of the file:
/// </para>
/// <list type="bullet">
/// <item>Only the instance on the LOCALLY OWNED player builds the UI, and it does so
/// lazily from <see cref="OnUpdate"/> rather than <c>OnAttach</c>: on a client
/// <see cref="Net.IsOwner"/> answers false for everything until the host's Welcome
/// lands, so ownership is not yet knowable at attach time.</item>
/// <item>The transcript is <b>static</b>. A multicast arrives on the SENDER's player
/// entity on every peer - which, on everyone but the sender, is not the entity holding
/// the UI. Collecting the lines in one process-wide list is what lets any instance's
/// <see cref="ReceiveChat"/> feed the one instance that draws them.</item>
/// </list>
/// <para>
/// The UI is created here rather than authored into <c>Arena.scene.toml</c>, matching
/// how <see cref="NameTag"/> builds its label: a prefab-borne script has no way to be
/// handed an <see cref="Entity"/> reference to a scene-placed element. The scene does
/// author the canvas these attach to, so everything in the HUD shares one.
/// </para>
/// <para>
/// <b>Idle behaviour.</b> The transcript and the input box are hidden between
/// conversations, not merely dimmed to nothing. Hiding is what takes the input box out of
/// the UI navigation set: left present, the mouse resting anywhere near the bottom-left
/// corner focuses it, and a focused element is what the character controller stands still
/// for. So "fades out when idle" and "does not quietly disable the game" are the same
/// change here, not two.
/// </para>
/// <para>
/// The transcript also carries the session's own announcements - who joined, who
/// left - written by the host through <see cref="Announce"/>. They are not a second
/// mechanism: they are ordinary chat lines the host composed itself, delivered by the
/// same <see cref="ReceiveChat"/> multicast, so there is exactly one path that puts
/// text in front of a player and one set of delivery rules to get right.
/// </para>
/// </remarks>
public sealed class ChatBox : EntityScript
{
    /// <summary>How many lines of transcript are shown at once. Older lines scroll off
    /// the top and stay reachable in the scrollback.</summary>
    public const int MaxLines = 8;

    /// <summary>How many lines are KEPT. Past this the oldest is dropped, so a long
    /// session cannot grow the transcript without bound.</summary>
    public const int ScrollbackLines = 200;

    /// <summary>Longest message accepted, in characters.</summary>
    public const int MaxMessageLength = 120;

    /// <summary>Gap between the chat block and the screen edges, in pixels.</summary>
    public float Margin = 16.0f;

    /// <summary>Width shared by the input box and the transcript, in pixels.</summary>
    public float Width = 560.0f;

    /// <summary>Height of the input box, in pixels.</summary>
    public float InputHeight = 32.0f;

    /// <summary>Vertical gap between the transcript and the input box, in pixels.</summary>
    public float Spacing = 8.0f;

    /// <summary>Transcript font size, in pixels. <see cref="LineHeight"/> should stay
    /// comfortably above this or eight lines will not fit the rect.</summary>
    public float LogFontSize = 16.0f;

    /// <summary>Vertical space budgeted per transcript line, in pixels.</summary>
    public float LineHeight = 20.0f;

    /// <summary>How long the chat stays fully visible after the last thing happened.</summary>
    public float HoldSeconds = 6.0f;

    /// <summary>How long it then takes to fade away.</summary>
    public float FadeSeconds = 1.2f;

    private static readonly Vector4 LogColor = new(0.90f, 0.92f, 1.00f, 1.0f);
    private static readonly Vector4 UnreadColor = new(1.00f, 0.729f, 0.310f, 1.0f);
    private static readonly Vector4 ScrollColor = new(0.478f, 0.518f, 0.596f, 1.0f);

    /// <summary>One transcript entry: when this peer received it, and what it said.</summary>
    /// <remarks>
    /// The time is stamped ON RECEIPT, by the peer showing it, not carried on the wire.
    /// A sender's clock is not this player's clock and there is no session clock to
    /// convert between them, so a transmitted timestamp would be a number from somebody
    /// else's machine presented as if it were local. "When I saw this" is a claim this
    /// peer can actually make.
    /// </remarks>
    private readonly record struct ChatLine(string Time, string Body)
    {
        public override string ToString() => $"[{Time}] {Body}";
    }

    // Process-wide transcript, shared by every ChatBox instance - see the remarks above
    // for why an instance field could not work. `Revision` is bumped on every append so
    // the drawing instance rebuilds the joined string only when there is something new.
    private static readonly List<ChatLine> s_lines = new();
    private static int s_revision;

    // Lines that have arrived since the local player last opened the box. Static for the
    // same reason the transcript is: ReceiveChat runs on the SENDER's entity, which on
    // everyone but the sender is not the instance drawing anything.
    private static int s_unread;

    private Entity _input;
    private Entity _log;
    private Entity _badge;
    private Entity _scrollHint;

    private int _drawnRevision = -1;
    private int _drawnScroll = -1;
    private bool _wasEditing;

    // How far back the transcript is scrolled, in lines from the newest. 0 is "following
    // the conversation", which is what an unread count is counted against.
    private int _scroll;

    // Seconds since anything happened. Drives the fade, and nothing else does - a single
    // timer is why the transcript, the input box and the hint can never disagree about
    // whether the chat is currently in use.
    private float _idle;

    // Frames the box stays enabled waiting for the edit it was just asked to begin. The
    // request Ui.BeginEdit leaves is picked up by the text box system on its NEXT tick, so
    // without a short grace the box would be switched off again on the frame it was asked
    // to open and the request would be discarded as stale. Bounded rather than a plain
    // flag: a request that never takes must not leave the box on screen forever.
    private int _openingFrames;

    /// <summary>True while this player's chat box owns the keyboard.</summary>
    /// <remarks>
    /// There is deliberately no static mirror of this. One used to exist, so the session
    /// director could ask "is somebody typing" before reading Escape, and it was a defect:
    /// the flag was written at the end of this script's update and read at the start of
    /// another's, so which script ran first decided whether one Escape cancelled a message
    /// or cancelled the whole session. Keys that a UI element handles are now consumed by
    /// the engine before any script runs, which answers the question without anybody
    /// having to ask it.
    /// </remarks>
    public bool IsTyping => _input.IsValid && Ui.IsEditing(_input);

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        // Remote players run this script too (it is on the prefab); only the peer that
        // owns a player gets to type as them. Net.IsOwner is true offline, so a solo
        // editor session still gets a working chat box.
        if (!Net.IsOwner(Self))
        {
            return;
        }

        if (!_input.IsValid)
        {
            BuildUi();
        }

        PumpInput();
        PumpScroll();
        RefreshLog();
        TickVisibility(deltaTime);
    }

    /// <inheritdoc/>
    public override void OnDetach()
    {
        if (!_input.IsValid)
        {
            // A remote player's instance never built anything and must not clear the
            // transcript when its owner disconnects.
            return;
        }

        _input.Destroy();
        foreach (Entity element in new[] { _log, _badge, _scrollHint })
        {
            if (element.IsValid)
            {
                element.Destroy();
            }
        }

        // The local player is destroyed when the session ends (leaving the arena, or
        // the host dropping). Clearing here rather than on build means a fresh arena
        // starts empty without racing a line that arrived before the local player had
        // finished spawning.
        s_lines.Clear();
        s_unread = 0;
        s_revision++;
    }

    // ── UI ──────────────────────────────────────────────────────────────────────

    /// <summary>Create the four elements, anchored to the screen's bottom-left corner.</summary>
    /// <remarks>
    /// Anchoring is explicit because new UI elements are CENTRE-anchored and
    /// <see cref="Ui.SetRect"/>'s x/y are an offset FROM the anchor, not an absolute
    /// position - left at the default, everything would sit in the middle of the screen.
    /// UI space has Y=0 at the TOP, so the bottom-left corner is (0, 1) and the offsets
    /// that walk up from it are negative.
    /// </remarks>
    private void BuildUi()
    {
        Vector2 bottomLeft = new(0.0f, 1.0f);
        // Top-left pivot, so SetRect's x/y is the element's own top-left corner and the
        // arithmetic below is a straight stack rather than half-height corrections.
        Vector2 topLeftPivot = Vector2.Zero;

        float logHeight = LineHeight * MaxLines;
        float inputTop = -(Margin + InputHeight);
        float logTop = inputTop - Spacing - logHeight;

        _scrollHint = Ui.CreateText();
        Ui.SetAnchors(_scrollHint, bottomLeft, bottomLeft);
        Ui.SetPivot(_scrollHint, topLeftPivot);
        Ui.SetRect(_scrollHint, Margin, logTop - LineHeight, Width, LineHeight);
        Style(_scrollHint, LogFontSize - 2.0f, UiHAlign.Left);
        Ui.SetTextColor(_scrollHint, ScrollColor);
        _scrollHint.SetActive(false);

        _log = Ui.CreateText();
        Ui.SetAnchors(_log, bottomLeft, bottomLeft);
        Ui.SetPivot(_log, topLeftPivot);
        Ui.SetRect(_log, Margin, logTop, Width, logHeight);
        Style(_log, LogFontSize, UiHAlign.Left);
        Ui.SetTextWrap(_log, true);
        // Bottom-aligned so the newest line sits just above the input box and a
        // half-full transcript grows upward instead of hanging in mid-air.
        Ui.SetTextAlign(_log, UiHAlign.Left, UiVAlign.Bottom);
        Ui.SetTextColor(_log, LogColor);
        Ui.SetText(_log, "");

        // The one element that survives the fade: it is the whole reason a player who was
        // busy elsewhere knows to open the chat at all.
        _badge = Ui.CreateText();
        Ui.SetAnchors(_badge, bottomLeft, bottomLeft);
        Ui.SetPivot(_badge, topLeftPivot);
        Ui.SetRect(_badge, Margin, inputTop, Width, InputHeight);
        Style(_badge, LogFontSize, UiHAlign.Left);
        Ui.SetTextColor(_badge, UnreadColor);
        _badge.SetActive(false);

        _input = Ui.CreateTextBox();
        Ui.SetAnchors(_input, bottomLeft, bottomLeft);
        Ui.SetPivot(_input, topLeftPivot);
        Ui.SetRect(_input, Margin, inputTop, Width, InputHeight);
        Ui.SetPlaceholder(_input, "Say something, Enter to send, Escape to cancel");
        Ui.SetContentType(_input, UiContentType.Any);
        // Refused at the keystroke now that a script-created box can be told its cap,
        // instead of being clipped at Enter by Sanitize and leaving the player wondering
        // where the rest of their sentence went. Sanitize still enforces it on the way in
        // AND on the host: a length the sender's UI honoured is not a length the host can
        // take on trust.
        Ui.SetMaxLength(_input, MaxMessageLength);
        Ui.SetFont(_input, "IBMPlexMono-Italic");
        Ui.SetFontSize(_input, LogFontSize);
        Ui.SetTextBoxText(_input, "");
        _input.SetActive(false);
    }

    private static void Style(Entity element, float fontSize, UiHAlign align)
    {
        Ui.SetFont(element, "IBMPlexMono-Italic");
        Ui.SetFontSize(element, fontSize);
        Ui.SetTextAlign(element, align, UiVAlign.Middle);
        Ui.SetTextWrap(element, false);
    }

    /// <summary>Push the visible window of the transcript into the label, but only when
    /// it has changed.</summary>
    private void RefreshLog()
    {
        if (!_log.IsValid || (_drawnRevision == s_revision && _drawnScroll == _scroll))
        {
            return;
        }
        _drawnRevision = s_revision;
        _drawnScroll = _scroll;

        int end = s_lines.Count - _scroll;
        int start = System.Math.Max(0, end - MaxLines);
        StringBuilder builder = new();
        for (int i = start; i < end; i++)
        {
            if (builder.Length > 0)
            {
                builder.Append('\n');
            }
            builder.Append(s_lines[i].ToString());
        }
        Ui.SetText(_log, builder.ToString());
    }

    // ── Visibility ──────────────────────────────────────────────────────────────

    /// <summary>Fade the chat out when nothing is happening, and take the input box out
    /// of the UI entirely while it is gone.</summary>
    /// <remarks>
    /// Hidden rather than left at zero alpha, and that is the load-bearing half: an
    /// invisible-but-present text box is still a navigation candidate, so a mouse resting
    /// near the corner would focus it and the character would stop moving for reasons
    /// nothing on screen explains. A disabled subtree is not a candidate at all.
    /// </remarks>
    private void TickVisibility(float deltaTime)
    {
        bool busy = IsTyping || _scroll > 0;
        if (busy)
        {
            _idle = 0.0f;
        }
        else
        {
            _idle += deltaTime;
        }

        float alpha = _idle <= HoldSeconds
            ? 1.0f
            : System.Math.Max(0.0f, 1.0f - (_idle - HoldSeconds) / System.Math.Max(FadeSeconds, 0.0001f));

        if (_log.IsValid)
        {
            _log.SetActive(alpha > 0.0f && s_lines.Count > 0);
            Ui.SetTextColor(_log, new Vector4(LogColor.X, LogColor.Y, LogColor.Z, alpha));
        }
        if (_scrollHint.IsValid)
        {
            bool show = _scroll > 0;
            _scrollHint.SetActive(show);
            if (show)
            {
                Ui.SetText(_scrollHint, $"-- scrolled back {_scroll} line{(_scroll == 1 ? "" : "s")}, PgDn to catch up --");
            }
        }
        // THE INPUT BOX EXISTS ONLY WHILE IT IS BEING TYPED IN, and that is a gameplay
        // rule rather than a cosmetic one. It is the arena's only focusable element, and a
        // focusable element present during play is one the mouse can drift onto, or that a
        // direction key seeds focus onto - either of which takes the keyboard, which is
        // what the character controller stands still for. A box that only exists between
        // Enter and Enter cannot do that.
        if (_input.IsValid)
        {
            _openingFrames = System.Math.Max(0, _openingFrames - 1);
            _input.SetActive(IsTyping || _openingFrames > 0);
        }
        if (_badge.IsValid)
        {
            // The line where the input box would be, when there is no input box: an unread
            // count for the player who is not looking, and otherwise the affordance that
            // says the chat is there at all. Plain text, never selectable - see above.
            string message = s_unread > 0
                ? $"{s_unread} new message{(s_unread == 1 ? "" : "s")} - Enter to read"
                : "Enter to chat";
            bool show = !IsTyping && _openingFrames == 0 && (s_unread > 0 || alpha > 0.0f);
            _badge.SetActive(show);
            if (show)
            {
                Ui.SetText(_badge, message);
                Ui.SetTextColor(_badge, s_unread > 0
                    ? UnreadColor
                    : new Vector4(ScrollColor.X, ScrollColor.Y, ScrollColor.Z, alpha));
            }
        }
    }

    // ── Input ───────────────────────────────────────────────────────────────────

    /// <summary>Enter opens the box; Enter again sends what is in it.</summary>
    /// <remarks>
    /// <para>
    /// The submit test comes first and the open test is its <c>else</c> branch on
    /// purpose. Committing a field ends editing in the same frame it reports
    /// <see cref="Ui.WasSubmitted"/>, so an independent "not editing and Enter is down"
    /// test would see the very keypress that just sent the message and reopen the box
    /// with it.
    /// </para>
    /// <para>
    /// Opening ENABLES the box before asking it to edit. It is switched off while idle,
    /// and <see cref="Ui.BeginEdit"/> leaves a one-shot request the text box system picks
    /// up on its next tick - by which time this has re-enabled it, so the request lands
    /// rather than being discarded as stale.
    /// </para>
    /// <para>
    /// Closing gives the keyboard back. A focused selectable is activated by Space as much
    /// as by Enter, so a box that stayed focused after sending would make the next jump
    /// open the chat instead. The chat owns the keyboard only between Enter and Enter.
    /// </para>
    /// </remarks>
    private void PumpInput()
    {
        if (Ui.WasSubmitted(_input))
        {
            Submit();
        }
        else if (!Ui.IsEditing(_input) && Input.IsKeyPressed(Key.Enter) && !Ui.HasFocus)
        {
            Open();
        }

        bool editing = Ui.IsEditing(_input);
        // Guarded on the box still holding focus, so a click that moved focus somewhere
        // else on the way out is not undone.
        if (_wasEditing && !editing && Ui.IsFocused(_input))
        {
            Ui.ClearFocus();
        }
        _wasEditing = editing;
    }

    /// <summary>Show the box, take the keyboard, and mark everything as read.</summary>
    /// <remarks>
    /// Opening the chat is the one unambiguous signal that the player is looking at it,
    /// which is why it - and not the arrival of a message, or the transcript happening to
    /// be visible - is what clears the unread count.
    /// </remarks>
    private void Open()
    {
        s_unread = 0;
        _idle = 0.0f;
        _scroll = 0;
        _openingFrames = 3;
        _input.SetActive(true);
        Ui.BeginEdit(_input);
    }

    /// <summary>PageUp/PageDown walk the transcript back through the scrollback.</summary>
    /// <remarks>
    /// Those two keys specifically because they are the only ones free in both states: the
    /// arrows move the caret while a message is being typed and move UI focus while it is
    /// not, and W/S/Up/Down are the character's. A scrollback that only worked while
    /// typing would be no use to the player who just saw an unread badge.
    /// </remarks>
    private void PumpScroll()
    {
        int hidden = System.Math.Max(0, s_lines.Count - MaxLines);
        if (Input.IsKeyPressed(Key.PageUp))
        {
            _scroll = System.Math.Min(hidden, _scroll + MaxLines / 2);
        }
        if (Input.IsKeyPressed(Key.PageDown))
        {
            _scroll = System.Math.Max(0, _scroll - MaxLines / 2);
        }
        // A line dropping off the far end of the scrollback shortens the transcript under
        // a reader who is parked in it; clamping here keeps the view inside the buffer
        // instead of showing a window that has walked off the top.
        _scroll = System.Math.Min(_scroll, hidden);
    }

    /// <summary>Send whatever was committed, then empty the box either way.</summary>
    private void Submit()
    {
        string message = Sanitize(Ui.GetTextBoxText(_input));
        // Always cleared, including on the empty-message and refused-call paths: a box
        // that keeps its contents after Enter reads as "that did not send" even when it
        // did, and leaves the next message appended to the last one.
        Ui.SetTextBoxText(_input, "");
        _idle = 0.0f;
        if (message.Length == 0)
        {
            return;
        }

        if (!Net.Call(Self, nameof(SendChat), message))
        {
            // The only way this refuses here is an entity with no net id yet, i.e. a
            // message typed in the handful of frames before the spawn round trip
            // finished. Worth saying out loud rather than losing silently.
            Debug.LogWarning("Whisper: chat message dropped, the local player is not replicated yet");
        }
    }

    /// <summary>
    /// Reduce a typed string to something safe to put on the wire and in a font: printable
    /// ASCII only, collapsed whitespace trimmed off the ends, and clipped to
    /// <see cref="MaxMessageLength"/>.
    /// </summary>
    /// <remarks>
    /// The ASCII filter is a hard requirement of the font pipeline (non-ASCII has no
    /// baked glyph), and dropping newlines specifically matters because the transcript
    /// is joined with <c>\n</c> - one pasted newline would otherwise count as a single
    /// entry while occupying two of the eight visible lines.
    /// </remarks>
    private static string Sanitize(string raw)
    {
        StringBuilder builder = new(raw.Length);
        foreach (char c in raw)
        {
            if (c >= ' ' && c <= '~')
            {
                builder.Append(c);
            }
            if (builder.Length >= MaxMessageLength)
            {
                break;
            }
        }
        return builder.ToString().Trim();
    }

    /// <summary>Local wall-clock time, as HH:MM, built by hand rather than formatted.</summary>
    /// <remarks>A culture-aware format would produce a 12-hour clock, an AM/PM marker, or
    /// a non-ASCII separator depending on the machine, and the font bakes ASCII only.</remarks>
    private static string Stamp()
    {
        System.DateTime now = System.DateTime.Now;
        return $"{now.Hour:00}:{now.Minute:00}";
    }

    // ── Networking ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Client -> host: "say this as me". The host is the only peer that decides who a
    /// message came from, so the sender supplies the body and nothing else.
    /// </summary>
    /// <remarks>
    /// Declared here, on the player, because the host rejects a server RPC aimed at an
    /// entity the sending connection does not own - see the remarks on the class.
    /// <see cref="EntityScript.Self"/> inside this method is the SENDER's player entity
    /// on whichever peer is running it, which is exactly the entity whose replicated
    /// name should be on the line.
    /// </remarks>
    /// <param name="message">The raw body, re-sanitised here: the sending client is the
    /// one peer whose sanitising the host cannot take on trust.</param>
    [NetRpc(NetRpcTarget.Server)]
    public void SendChat(string message)
    {
        string clean = Sanitize(message);
        if (clean.Length == 0)
        {
            return;
        }
        // Net.GetPlayerName reads the replicated NetPlayer.displayName the sender's own
        // machine authored and replication carried here, so attribution is the host's
        // copy of the name rather than anything the sender put in this call.
        Net.Call(Self, nameof(ReceiveChat), $"{Net.GetPlayerName(Self)}: {clean}");
    }

    /// <summary>
    /// Host -> everyone (the host included): append one finished, already-attributed
    /// line to the transcript.
    /// </summary>
    /// <remarks>
    /// Writes the process-wide list rather than this instance's UI, because this runs on
    /// the sender's player entity on every peer - see the class remarks. The host runs it
    /// locally too, which is what puts the host's own messages in its own transcript.
    /// </remarks>
    /// <param name="line">A complete "Name: message" line, formatted by the host.</param>
    [NetRpc(NetRpcTarget.Multicast)]
    public void ReceiveChat(string line)
    {
        s_lines.Add(new ChatLine(Stamp(), line));
        while (s_lines.Count > ScrollbackLines)
        {
            s_lines.RemoveAt(0);
        }
        s_unread++;
        s_revision++;
    }

    /// <summary>
    /// Host only: put a session announcement - somebody joining, somebody leaving - in
    /// every peer's transcript, on the same multicast the chat itself rides.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <paramref name="carrier"/> has to be an entity the HOST owns that carries this
    /// script, in practice the host's own player. Two independent rules force that: a
    /// multicast is refused outright unless it originates on the host, and the call is
    /// addressed on the wire by the carrier's net id, so the carrier must be replicated
    /// as well. <see cref="WhisperSession"/> has the third reason - a leave
    /// announcement outlives the entity it is about.
    /// </para>
    /// <para>
    /// Returning false means "not yet", not "failed". A player entity that has only
    /// just spawned has no live <see cref="ChatBox"/> instance for the local half of
    /// the multicast to land on, and the framework drops an RPC aimed at a script that
    /// has not been instantiated - reporting success there would lose the line
    /// silently. The caller is expected to hold it and try again on a later frame.
    /// </para>
    /// </remarks>
    /// <param name="carrier">The host-owned, replicated player the line rides on.</param>
    /// <param name="line">The finished line, e.g. "Alice joined". Sanitised here, so a
    /// display name carrying something the font has no glyph for cannot reach the
    /// transcript by the back door that <see cref="SendChat"/> already closes.</param>
    /// <returns>True once the line has been routed and the caller can forget it.</returns>
    public static bool Announce(Entity carrier, string line)
    {
        if (!carrier.IsValid || carrier.GetScript<ChatBox>() is null)
        {
            return false;
        }
        string clean = Sanitize(line);
        if (clean.Length == 0)
        {
            // Nothing printable survived, so there is nothing to send - but report it
            // done, or a caller queueing this line would retry it forever.
            return true;
        }
        return Net.Call(carrier, nameof(ReceiveChat), clean);
    }
}
