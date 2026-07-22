using System;
using System.Collections.Generic;
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
    private const float PortraitSize = BoxH - 2f * Pad;

    private const string BodyFontName = "IBMPlexMono-Italic";
    private const string DisplayFontName = "PixelStorm";

    private Entity _canvas, _panel, _rule, _portrait, _speaker, _body, _hint;
    private bool _built;
    private float _bodyLeft = Pad;

    // Inline rich-text spans: a line with >1 run (i.e. it has [tag] markup) is laid out as one text
    // element per run on a monospace grid (IBM Plex Mono => fixed cell width). A plain/whole-line node
    // (1 run) keeps the single _body element with the engine's own word-wrap. Keep spanned lines short.
    private const int MaxRuns = 16;
    private const float MonoAdvance = 0.60f; // IBM Plex Mono advance in ems
    private const float LineH = BodyFont + 6f;
    private readonly Entity[] _runUi = new Entity[MaxRuns];
    private bool _multiRun;

    public bool Active { get; private set; }
    private DialogueGraph? _graph;
    private DialogueNode? _node;
    private float _reveal;      // characters revealed so far (grows on UnscaledTime)
    private bool _fullShown;

    // Choices (created at runtime; runtime elements can't be given the scene-authored Selectable nav
    // component, so the runner tracks the focused index itself and styles it like the menu markers).
    private const int MaxChoices = 6;
    private const float ChoiceTop = 112f;
    private const float ChoiceH = 30f;
    private const float ChoiceFont = 22f;
    private static readonly Vector4 ChoiceDim = new(0.60f, 0.66f, 0.72f, 0.80f);
    private readonly Entity[] _choiceUi = new Entity[MaxChoices];
    private readonly List<DialogueChoice> _visible = new();
    private int _focus;
    private bool _choicesShown;

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

        // Portrait region on the left (shown per node when it declares a portrait).
        _portrait = Ui.CreateImage(_canvas);
        _portrait.SetParent(_panel);
        Ui.SetAnchors(_portrait, new Vector2(0f, 0f), new Vector2(0f, 0f));
        Ui.SetPivot(_portrait, new Vector2(0f, 0f));
        Ui.SetRect(_portrait, Pad, Pad, PortraitSize, PortraitSize);
        Ui.SetImageColor(_portrait, new Vector4(0.06f, 0.09f, 0.13f, 1f)); // frame/backing behind the art
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
        // Rich-text material: always applied; effectId 0 (Normal) renders as plain SDF text, so no
        // separate clear path is needed when a line has no effect.
        Ui.SetMaterial(_body, "ui_dialogue_text");
        Ui.SetMaterialColors(_body, Vector4.Zero, Accent);

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

        // Choice slots, stacked below the body (single line each, no wrap).
        for (int i = 0; i < MaxChoices; i++)
        {
            Entity c = Ui.CreateText(_canvas, "");
            c.SetParent(_panel);
            Ui.SetAnchors(c, new Vector2(0f, 0f), new Vector2(1f, 0f));
            Ui.SetOffsets(c, new Vector2(_bodyLeft, ChoiceTop + i * ChoiceH), new Vector2(-Pad, ChoiceTop + (i + 1) * ChoiceH));
            Ui.SetFont(c, BodyFontName);
            Ui.SetFontSize(c, ChoiceFont);
            Ui.SetTextAlign(c, UiHAlign.Left, UiVAlign.Top);
            Ui.SetTextWrap(c, false);
            c.SetActive(false);
            _choiceUi[i] = c;
        }

        // Run slots for inline-span lines (mono grid, no wrap, one material per run).
        for (int i = 0; i < MaxRuns; i++)
        {
            Entity r = Ui.CreateText(_canvas, "");
            r.SetParent(_panel);
            Ui.SetAnchors(r, new Vector2(0f, 0f), new Vector2(0f, 0f));
            Ui.SetPivot(r, new Vector2(0f, 0f));
            Ui.SetFont(r, BodyFontName);
            Ui.SetFontSize(r, BodyFont);
            Ui.SetTextColor(r, BodyCol);
            Ui.SetTextAlign(r, UiHAlign.Left, UiVAlign.Top);
            Ui.SetTextWrap(r, false);
            Ui.SetMaterial(r, "ui_dialogue_text");
            Ui.SetMaterialColors(r, Vector4.Zero, Accent);
            r.SetActive(false);
            _runUi[i] = r;
        }

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
        HideChoices();
        _node = _graph?.NodeOrNull(nodeId);
        if (_node == null) { End(); return; }
        _reveal = 0f;
        _fullShown = false;
        _hint.SetActive(true);

        // Portrait: show + widen-inset the text column when the node has one, else hide + fill.
        if (!string.IsNullOrEmpty(_node.Portrait))
        {
            Ui.SetImageTexture(_portrait, "project://assets/" + _node.Portrait);
            _portrait.SetActive(true);
            _bodyLeft = Pad + PortraitSize + Pad;
        }
        else
        {
            _portrait.SetActive(false);
            _bodyLeft = Pad;
        }
        RelayoutText(_bodyLeft);

        // Inline spans: >1 run means the line has [tag] markup -> lay out per-run on the mono grid,
        // otherwise use the single wrapped body element.
        HideRuns();
        _multiRun = _node.Runs.Count > 1;
        if (_multiRun)
        {
            _body.SetActive(false);
            int count = Math.Min(_node.Runs.Count, MaxRuns);
            for (int i = 0; i < count; i++) { _runUi[i].SetActive(true); Ui.SetText(_runUi[i], ""); }
        }
        else
        {
            _body.SetActive(true);
        }

        Ui.SetText(_speaker, _node.Speaker);
        Ui.SetText(_body, "");
    }

    // Re-place the speaker/body/choice columns to start at bodyLeft (shifts right of a portrait).
    private void RelayoutText(float bodyLeft)
    {
        Ui.SetRect(_speaker, bodyLeft, Pad, 600f, SpeakerH);
        Ui.SetOffsets(_body, new Vector2(bodyLeft, Pad + SpeakerH + 6f), new Vector2(-Pad, BoxH - Pad));
        for (int i = 0; i < MaxChoices; i++)
        {
            Ui.SetOffsets(_choiceUi[i], new Vector2(bodyLeft, ChoiceTop + i * ChoiceH), new Vector2(-Pad, ChoiceTop + (i + 1) * ChoiceH));
        }
    }

    public override void OnUpdate(float dt)
    {
        if (!Active || _node == null) return;
        float udt = UnscaledDelta(Time.UnscaledTime);
        float strength = 0.6f + 0.4f * GameSettings.InkGlow;

        if (Input.IsKeyPressed(Key.Escape)) { End(); return; }

        bool tap = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.Enter)
                   || Input.IsMousePressed(MouseButton.Left);

        // Phase 1 - typewriter. A tap snaps the line to full. On the frame the line completes we
        // reveal choices (if any) and return, so the same tap never also advances/activates.
        if (!_fullShown)
        {
            _reveal += udt * CharsPerSec;
            int shown = Math.Min(_node.Text.Length, (int)_reveal);
            if (tap) { shown = _node.Text.Length; }
            RenderBody(shown, strength);
            if (shown >= _node.Text.Length)
            {
                _fullShown = true;
                if (_node.HasChoices) { ShowChoices(); }
            }
            return;
        }

        // Phase 2 - fully shown; keep the per-glyph effects (and any inline-run motion) animating.
        RenderBody(_node.Text.Length, strength);

        if (_choicesShown) { UpdateChoices(); return; }

        if (tap) { GoTo(_node.Goto); } // linear advance
    }

    // Draw the body up to `shown` characters, routing to the single wrapped element or the mono
    // run grid depending on whether the line has inline spans.
    private void RenderBody(int shown, float strength)
    {
        if (_multiRun) { UpdateRuns(shown, strength); return; }
        Ui.SetText(_body, _node!.Text.Substring(0, shown));
        InkEffect eff = _node.Runs.Count > 0 ? _node.Runs[0].Effect : _node.Effect;
        Ui.SetMaterialParams(_body, new Vector4(Time.UnscaledTime, (float)eff, strength, 0f));
    }

    // Lay out inline-span runs on a monospace grid (wrap whole runs at the panel edge) and reveal a
    // global character count across them in order. Recomputed each frame so it self-corrects once the
    // panel rect is resolved and the shake/wave motion stays live.
    private void UpdateRuns(int revealed, float strength)
    {
        float cellW = BodyFont * MonoAdvance;
        float bodyTop = Pad + SpeakerH + 6f;
        float panelW = Ui.GetRect(_panel).Z;
        float availW = panelW - _bodyLeft - Pad;
        int maxCols = availW > cellW ? (int)(availW / cellW) : 9999;

        int col = 0, row = 0, acc = 0;
        int count = Math.Min(_node!.Runs.Count, MaxRuns);
        for (int i = 0; i < count; i++)
        {
            TextRun r = _node.Runs[i];
            int len = r.Text.Length;
            if (col > 0 && col + len > maxCols) { col = 0; row++; }

            Entity e = _runUi[i];
            float x = _bodyLeft + col * cellW;
            float y = bodyTop + row * LineH;
            Ui.SetOffsets(e, new Vector2(x, y), new Vector2(x + len * cellW + 6f, y + LineH));
            int show = Math.Clamp(revealed - acc, 0, len);
            Ui.SetText(e, r.Text.Substring(0, show));
            Ui.SetMaterialParams(e, new Vector4(Time.UnscaledTime, (float)r.Effect, strength, 0f));

            col += len;
            acc += len;
        }
    }

    private void HideRuns()
    {
        _multiRun = false;
        for (int i = 0; i < MaxRuns; i++)
        {
            if (_runUi[i].IsValid) { _runUi[i].SetActive(false); }
        }
    }

    private void ShowChoices()
    {
        _visible.Clear();
        foreach (DialogueChoice c in _node!.Choices)
        {
            if (DialogueState.Evaluate(c.If)) { _visible.Add(c); }
        }
        _focus = 0;
        _choicesShown = true;
        _hint.SetActive(false);
        for (int i = 0; i < MaxChoices; i++)
        {
            _choiceUi[i].SetActive(i < _visible.Count);
        }
    }

    private void HideChoices()
    {
        _choicesShown = false;
        for (int i = 0; i < MaxChoices; i++)
        {
            if (_choiceUi[i].IsValid) { _choiceUi[i].SetActive(false); }
        }
    }

    private void UpdateChoices()
    {
        int n = _visible.Count;
        if (n == 0) { GoTo(_node!.Goto); return; } // every choice gated out -> fall through

        if (Input.IsKeyPressed(Key.Up) || Input.IsKeyPressed(Key.W)) { _focus = (_focus - 1 + n) % n; }
        if (Input.IsKeyPressed(Key.Down) || Input.IsKeyPressed(Key.S)) { _focus = (_focus + 1) % n; }
        for (int i = 0; i < n; i++)
        {
            if (Ui.IsHovered(_choiceUi[i])) { _focus = i; }
        }

        float breathe = 0.72f + 0.28f * MathF.Sin(Time.UnscaledTime * 4.5f);
        for (int i = 0; i < n; i++)
        {
            bool on = i == _focus;
            Ui.SetText(_choiceUi[i], (on ? "> " : "  ") + _visible[i].Text);
            Ui.SetTextColor(_choiceUi[i], on ? new Vector4(Accent.X, Accent.Y, Accent.Z, breathe) : ChoiceDim);
        }

        bool activate = Input.IsKeyPressed(Key.Enter) || Input.IsKeyPressed(Key.Space)
                        || (Input.IsMousePressed(MouseButton.Left) && Ui.IsHovered(_choiceUi[_focus]));
        if (activate) { Choose(_focus); }
    }

    private void Choose(int i)
    {
        DialogueChoice c = _visible[i];
        if (!string.IsNullOrEmpty(c.Set)) { DialogueState.SetFlag(c.Set); }
        GoTo(c.Goto); // GoTo hides the choices
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
        HideChoices();
        HideRuns();
        _lastU = -1f;
    }
}
