using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>The one persistent dialogue presenter. Builds its ink box UI at runtime, freezes gameplay
/// while a conversation runs (Time.Pause), and animates on Time.UnscaledTime. Attach to a host entity
/// in every gameplay scene (or a DontDestroyOnLoad host). Linear playback here; choices/portrait/
/// rich-text/animation are layered on by later tasks.</summary>
public sealed class DialogueRunner : EntityScript
{
    // Palette (matches the menu theme).
    private static readonly Vector4 InkPanel = new(0.02f, 0.03f, 0.05f, 0.94f);
    private static readonly Vector4 Accent = GameSettings.Accent;
    private static readonly Vector4 BodyCol = new(0.93f, 0.95f, 0.97f, 1f);

    // Layout (screen/panel px; box hugs the bottom).
    private const float Margin = 44f;   // panel inset from screen edges
    private const float BoxH = 210f;    // panel height
    private const float Pad = 26f;      // inner padding
    private const float RuleH = 3f;     // accent rule thickness
    private const float SpeakerFont = 22f;
    private const float BodyFont = 26f;
    private const float SpeakerH = 30f;
    private const float CharsPerSec = 42f;

    private const string BodyFontName = "IBMPlexMono-Italic";
    private const string DisplayFontName = "PixelStorm";

    private Entity _canvas, _panel, _rule, _portrait, _speaker, _body, _hint;
    private bool _built;
    private float _bodyLeft = Pad;

    public bool Active { get; private set; }
    private DialogueGraph? _graph;
    private DialogueNode? _node;
    private float _reveal;      // characters revealed so far (grows on UnscaledTime)
    private bool _fullShown;

    public override void OnAttach()
    {
        Self.DontDestroyOnLoad();
        Dialogue.Register(this);
        Build();
        Hide();
    }

    public override void OnDetach() => Dialogue.Unregister(this);

    private void Build()
    {
        if (_built) return;
        _canvas = Ui.CreateCanvas();

        // Panel: bottom-anchored stretch (anchor Y=1 is the bottom edge; offsets go up = negative Y).
        _panel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_panel, new Vector2(0f, 1f), new Vector2(1f, 1f));
        Ui.SetOffsets(_panel, new Vector2(Margin, -(Margin + BoxH)), new Vector2(-Margin, -Margin));
        Ui.SetImageColor(_panel, InkPanel);
        Ui.SetImageCornerRadius(_panel, 8f);

        // Accent rule along the panel's top edge.
        _rule = Ui.CreateImage(_canvas);
        _rule.SetParent(_panel);
        Ui.SetAnchors(_rule, new Vector2(0f, 0f), new Vector2(1f, 0f));
        Ui.SetOffsets(_rule, new Vector2(0f, 0f), new Vector2(0f, RuleH));
        Ui.SetImageColor(_rule, Accent);

        // Portrait region on the left (reserved; wired in Task 6, hidden for now).
        float portraitSize = BoxH - 2f * Pad;
        _portrait = Ui.CreateImage(_canvas);
        _portrait.SetParent(_panel);
        Ui.SetAnchors(_portrait, new Vector2(0f, 0f), new Vector2(0f, 0f));
        Ui.SetPivot(_portrait, new Vector2(0f, 0f));
        Ui.SetRect(_portrait, Pad, Pad, portraitSize, portraitSize);
        Ui.SetImageCornerRadius(_portrait, 6f);
        _portrait.SetActive(false);

        // Speaker name (display font, accent).
        _speaker = Ui.CreateText(_canvas, "");
        _speaker.SetParent(_panel);
        Ui.SetAnchors(_speaker, new Vector2(0f, 0f), new Vector2(0f, 0f));
        Ui.SetPivot(_speaker, new Vector2(0f, 0f));
        Ui.SetRect(_speaker, _bodyLeft, Pad, 600f, SpeakerH);
        Ui.SetFont(_speaker, DisplayFontName);
        Ui.SetFontSize(_speaker, SpeakerFont);
        Ui.SetTextColor(_speaker, Accent);
        Ui.SetTextAlign(_speaker, UiHAlign.Left, UiVAlign.Top);

        // Body (mono italic, wraps to the panel width minus insets).
        _body = Ui.CreateText(_canvas, "");
        _body.SetParent(_panel);
        Ui.SetAnchors(_body, new Vector2(0f, 0f), new Vector2(1f, 0f));
        Ui.SetOffsets(_body, new Vector2(_bodyLeft, Pad + SpeakerH + 6f), new Vector2(-Pad, BoxH - Pad));
        Ui.SetFont(_body, BodyFontName);
        Ui.SetFontSize(_body, BodyFont);
        Ui.SetTextColor(_body, BodyCol);
        Ui.SetTextAlign(_body, UiHAlign.Left, UiVAlign.Top);

        // Advance hint, bottom-right.
        _hint = Ui.CreateText(_canvas, "> continue");
        _hint.SetParent(_panel);
        Ui.SetAnchors(_hint, new Vector2(1f, 1f), new Vector2(1f, 1f));
        Ui.SetPivot(_hint, new Vector2(1f, 1f));
        Ui.SetRect(_hint, -Pad, -Pad, 200f, 22f);
        Ui.SetFont(_hint, BodyFontName);
        Ui.SetFontSize(_hint, 15f);
        Ui.SetTextColor(_hint, new Vector4(Accent.X, Accent.Y, Accent.Z, 0.55f));
        Ui.SetTextAlign(_hint, UiHAlign.Right, UiVAlign.Bottom);

        _built = true;
    }

    public void Begin(DialogueGraph graph)
    {
        _graph = graph;
        Active = true;
        _lastU = -1f;
        Time.Pause();
        Show();
        GoTo(graph.Start);
    }

    private void GoTo(string? nodeId)
    {
        _node = _graph?.NodeOrNull(nodeId);
        if (_node == null) { End(); return; }
        _reveal = 0f;
        _fullShown = false;
        Ui.SetText(_speaker, _node.Speaker);
        Ui.SetText(_body, "");
    }

    public override void OnUpdate(float dt)
    {
        if (!Active || _node == null) return;
        float udt = UnscaledDelta(Time.UnscaledTime);

        // Typewriter reveal.
        if (!_fullShown)
        {
            _reveal += udt * CharsPerSec;
            int shown = Math.Min(_node.Text.Length, (int)_reveal);
            Ui.SetText(_body, _node.Text.Substring(0, shown));
            if (shown >= _node.Text.Length) { _fullShown = true; }
        }

        // Escape ends the whole conversation; Space/Enter/Click advances (or snaps to full first).
        if (Input.IsKeyPressed(Key.Escape)) { End(); return; }
        bool advance = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.Enter)
                       || Input.IsMousePressed(MouseButton.Left);
        if (advance)
        {
            if (!_fullShown) { _reveal = _node.Text.Length; _fullShown = true; Ui.SetText(_body, _node.Text); }
            else { Next(); }
        }
    }

    private void Next()
    {
        // Linear only for now (choices land in Task 5).
        GoTo(_node!.Goto);
    }

    private void End()
    {
        Active = false;
        _graph = null;
        _node = null;
        Hide();
        Time.Resume();
    }

    // ── helpers ──
    private float _lastU = -1f;
    private float UnscaledDelta(float u)
    {
        float d = _lastU < 0f ? 0f : Math.Max(0f, u - _lastU);
        _lastU = u;
        return d;
    }

    private void Show()
    {
        if (!_built) return;
        _panel.SetActive(true);
        _rule.SetActive(true);
        _speaker.SetActive(true);
        _body.SetActive(true);
        _hint.SetActive(true);
    }

    private void Hide()
    {
        if (!_built) return;
        _panel.SetActive(false);
        _rule.SetActive(false);
        _portrait.SetActive(false);
        _speaker.SetActive(false);
        _body.SetActive(false);
        _hint.SetActive(false);
        _lastU = -1f;
    }
}
