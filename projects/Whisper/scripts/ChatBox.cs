using System.Collections.Generic;
using System.Numerics;
using System.Text;
using AetherCore;

namespace AetherGame;

/// <summary>
/// In-game text chat: a single-line input pinned to the bottom-left corner with a
/// scrolling eight-line transcript above it. Enter opens the box, Enter again sends,
/// and every peer sees the line attributed to the sender's replicated display name.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why this script lives on the player prefab.</b> The host only accepts a
/// <see cref="NetRpcTarget.Server"/> call aimed at an entity the SENDING connection
/// owns - the ownership gate documented at length on
/// <see cref="PlayerController.SubmitName"/>. A chat submission is exactly that kind
/// of call, so <see cref="SendChat"/> has to be declared on a script attached to the
/// caller's own player. Putting it on the scene-placed, host-owned <c>Session</c>
/// entity would compile, route, and then be silently dropped on arrival.
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
/// how <see cref="NameTag"/> builds its label. Two reasons: a prefab-borne script has
/// no way to be handed an <see cref="Entity"/> reference to a scene-placed element
/// (prefabs cannot carry scene references), and the arena has no canvas at all, so
/// authoring one would exist solely to hold these two elements.
/// <see cref="Ui.CreateTextBox"/> and <see cref="Ui.CreateText"/> with a default
/// canvas argument attach to the first canvas, creating one if none exists.
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
    /// <summary>How many lines of transcript are kept. Older lines fall off the top.</summary>
    public const int MaxLines = 8;

    /// <summary>Longest message accepted, in characters. Enforced here rather than by
    /// the text box's own <c>max_length</c> field: that field is authorable in a scene
    /// but has no SDK setter, so a script-created box cannot be told about it.</summary>
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

    // Process-wide transcript, shared by every ChatBox instance - see the remarks
    // above for why an instance field could not work. `Revision` is bumped on every
    // append so the drawing instance can rebuild the joined string only when there is
    // something new, instead of re-joining eight strings every frame.
    private static readonly List<string> s_lines = new();
    private static int s_revision;

    private Entity _input;
    private Entity _log;
    private int _drawnRevision = -1;

    /// <summary>
    /// True while this player's chat box owns the keyboard. <see cref="PlayerController"/>
    /// polls this to stand still: without it every letter of a message is also a
    /// movement key, and typing "add" walks the character across the arena.
    /// </summary>
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
        RefreshLog();
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
        if (_log.IsValid)
        {
            _log.Destroy();
        }

        // The local player is destroyed when the session ends (leaving the arena, or
        // the host dropping). Clearing here rather than on build means a fresh arena
        // starts empty without racing a line that arrived before the local player had
        // finished spawning.
        s_lines.Clear();
        s_revision++;
    }

    // ── UI ──────────────────────────────────────────────────────────────────────

    /// <summary>Create the two elements, anchored to the screen's bottom-left corner.</summary>
    /// <remarks>
    /// Anchoring is explicit because new UI elements are CENTRE-anchored and
    /// <see cref="Ui.SetRect"/>'s x/y are an offset FROM the anchor, not an absolute
    /// position - left at the default, both elements would sit in the middle of the
    /// screen. UI space has Y=0 at the TOP, so the bottom-left corner is (0, 1) and
    /// the offsets that walk up from it are negative.
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

        // Passing a default (invalid) canvas attaches to the first canvas in the scene,
        // creating one if none exists - the Arena scene has none, so whichever of this
        // and NameTag runs first is what actually makes the canvas.
        _log = Ui.CreateText();
        Ui.SetAnchors(_log, bottomLeft, bottomLeft);
        Ui.SetPivot(_log, topLeftPivot);
        Ui.SetRect(_log, Margin, logTop, Width, logHeight);
        Ui.SetFontSize(_log, LogFontSize);
        Ui.SetTextWrap(_log, true);
        // Bottom-aligned so the newest line sits just above the input box and a
        // half-full transcript grows upward instead of hanging in mid-air.
        Ui.SetTextAlign(_log, UiHAlign.Left, UiVAlign.Bottom);
        Ui.SetTextColor(_log, new Vector4(0.90f, 0.92f, 1.0f, 1.0f));
        Ui.SetText(_log, "");

        _input = Ui.CreateTextBox();
        Ui.SetAnchors(_input, bottomLeft, bottomLeft);
        Ui.SetPivot(_input, topLeftPivot);
        Ui.SetRect(_input, Margin, inputTop, Width, InputHeight);
        Ui.SetPlaceholder(_input, "Press Enter to chat");
        Ui.SetContentType(_input, UiContentType.Any);
        Ui.SetTextBoxText(_input, "");
    }

    /// <summary>Push the transcript into the label, but only when it has changed.</summary>
    private void RefreshLog()
    {
        if (_drawnRevision == s_revision || !_log.IsValid)
        {
            return;
        }
        _drawnRevision = s_revision;
        Ui.SetText(_log, string.Join("\n", s_lines));
    }

    // ── Input ───────────────────────────────────────────────────────────────────

    /// <summary>Enter opens the box; Enter again sends what is in it.</summary>
    /// <remarks>
    /// The submit test comes first and the open test is its <c>else</c> branch on
    /// purpose. Committing a field ends editing in the same frame it reports
    /// <see cref="Ui.WasSubmitted"/>, so an independent "not editing and Enter is down"
    /// test would see the very keypress that just sent the message and reopen the box
    /// with it.
    /// </remarks>
    private void PumpInput()
    {
        if (Ui.WasSubmitted(_input))
        {
            Submit();
        }
        else if (!Ui.IsEditing(_input) && Input.IsKeyPressed(Key.Enter))
        {
            Ui.BeginEdit(_input);
        }
    }

    /// <summary>Send whatever was committed, then empty the box either way.</summary>
    private void Submit()
    {
        string message = Sanitize(Ui.GetTextBoxText(_input));
        // Always cleared, including on the empty-message and refused-call paths: a box
        // that keeps its contents after Enter reads as "that did not send" even when it
        // did, and leaves the next message appended to the last one.
        Ui.SetTextBoxText(_input, "");
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

    // ── Networking ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Client -> host: "say this as me". The host is the only peer that decides who a
    /// message came from, so the sender supplies the body and nothing else.
    /// </summary>
    /// <remarks>
    /// Declared here, on the player, because the host rejects a server RPC aimed at an
    /// entity the sending connection does not own - see the remarks on the class and on
    /// <see cref="PlayerController.SubmitName"/>. <see cref="EntityScript.Self"/> inside
    /// this method is the SENDER's player entity on whichever peer is running it, which
    /// is exactly the entity whose replicated name should be on the line.
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
        // Net.GetPlayerName reads the replicated NetPlayer.displayName the client
        // reported through PlayerController.SubmitName, so attribution is the host's
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
        s_lines.Add(line);
        while (s_lines.Count > MaxLines)
        {
            s_lines.RemoveAt(0);
        }
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
