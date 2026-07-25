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
    // Lifted off pure black and fully opaque: the levels are lit now, so a near-black panel just
    // disappeared into the dark cave and the dialogue read as a frozen game.
    private static readonly Vector4 InkPanel = new(0.07f, 0.10f, 0.14f, 1.0f);
    private static readonly Vector4 InkRimEdge = new(0.24f, 0.62f, 0.74f, 1f); // wet teal-ink rim for the panel
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

    private Entity _canvas, _panel, _rule, _portrait, _speaker, _hint;
    private bool _built;
    private float _bodyLeft = Pad;

    // Per-glyph reveal cells: every body glyph is its own text element, laid out on a monospace
    // word-wrap grid (IBM Plex Mono => fixed cell width). Each cell carries its run's effect and a
    // per-glyph "birth" (0 = freshly revealed wet ink blob, 1 = dried crisp letter), so every letter
    // blooms in independently. This one path subsumes both plain lines and inline [tag] spans.
    private const int MaxCells = 192;
    private const float MonoAdvance = 0.60f; // IBM Plex Mono advance in ems
    private const float LineH = BodyFont + 6f;
    private const float BirthChars = 4.5f;   // reveal-distance (in glyphs) over which a letter dries
    private readonly Entity[] _cellUi = new Entity[MaxCells];
    private char[] _chars = System.Array.Empty<char>();
    private InkEffect[] _cellFx = System.Array.Empty<InkEffect>();
    private int _cellCount;

    // Animation (all on UnscaledTime): the box slides up on open and back down on close, each node's
    // speaker/portrait fade in, and choices stagger in one by one.
    private const float OpenDur = 0.30f;
    private const float CloseDur = 0.22f;
    private const float NodeFadeDur = 0.18f;
    private const float ChoiceStagger = 0.07f;
    private const float ChoiceFadeDur = 0.16f;

    public bool Active { get; private set; }
    private DialogueGraph? _graph;
    private DialogueNode? _node;
    private float _reveal;      // characters revealed so far (grows on UnscaledTime)
    private bool _fullShown;
    private float _boxT;        // 0 = tucked below the screen, 1 = seated
    private bool _closing;      // playing the close slide, then FinishClose
    private float _nodeT;       // per-node content fade-in
    private float _choiceStartU; // UnscaledTime when choices were shown (drives the stagger)

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

    // Exactly one runner is guaranteed by PlayerController, which spawns this prefab only when
    // Scene.Find turns up none live - so there is NO static singleton here. A static reference would
    // outlive a play/scene reload pointing at a destroyed entity, and the fresh runner would then
    // "see a duplicate" and delete itself, leaving the game with no runner at all.
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
        Ui.SetImageCornerRadius(_panel, 10f);
        // Brushed pixel-ink material (same one the menu/pause panels use): roughened edges + grain +
        // a wet rim, so the box reads as soaked ink instead of a flat rounded rectangle.
        Ui.SetMaterial(_panel, "ui_ink_ui");
        Ui.SetMaterialColors(_panel, Vector4.Zero, InkRimEdge);

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
        // Glitch-ink the speaker name like the title wordmark (occasional eerie flicker).
        Ui.SetMaterial(_speaker, "ui_glitch_text");
        Ui.SetMaterialColors(_speaker, Vector4.Zero, Accent);

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

        // Per-glyph cell pool (one text element per body glyph, mono grid, no wrap, ink material).
        for (int i = 0; i < MaxCells; i++)
        {
            Entity c = Ui.CreateText(_canvas, "");
            c.SetParent(_panel);
            Ui.SetAnchors(c, new Vector2(0f, 0f), new Vector2(0f, 0f));
            Ui.SetPivot(c, new Vector2(0f, 0f));
            Ui.SetFont(c, BodyFontName);
            Ui.SetFontSize(c, BodyFont);
            Ui.SetTextColor(c, BodyCol);
            Ui.SetTextAlign(c, UiHAlign.Left, UiVAlign.Top);
            Ui.SetTextWrap(c, false);
            Ui.SetMaterial(c, "ui_dialogue_text");
            Ui.SetMaterialColors(c, Vector4.Zero, Accent);
            c.SetActive(false);
            _cellUi[i] = c;
        }

        _built = true;
    }

    public void Begin(DialogueGraph graph)
    {
        _graph = graph;
        Active = true;
        _closing = false;
        _boxT = 0f;
        _lastU = -1f;
        Time.Pause();
        Show();
        ApplyBoxSlide(0f); // start tucked below the screen so it slides up
        GoTo(graph.Start);
    }

    private void GoTo(string? nodeId)
    {
        HideChoices();
        _node = _graph?.NodeOrNull(nodeId);
        if (_node == null) { RequestClose(); return; }
        Dialogue.CurrentNodeId = nodeId; // graph editor highlights this node during play
        _reveal = 0f;
        _fullShown = false;
        _nodeT = 0f; // restart the per-node fade-in
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
        BuildCells();
        Ui.SetText(_speaker, _node.Speaker);
    }

    // Flatten the node's styled runs into per-glyph cells (char + effect), clamped to the pool. Positions
    // and per-glyph birth are computed each frame in RenderCells; here we just seed the data and clear
    // any cells left over from the previous node.
    private void BuildCells()
    {
        for (int i = 0; i < MaxCells; i++) { if (_cellUi[i].IsValid) { _cellUi[i].SetActive(false); } }

        int total = 0;
        foreach (TextRun r in _node!.Runs) { total += r.Text.Length; }
        total = Math.Min(total, MaxCells);
        if (_chars.Length != total) { _chars = new char[total]; _cellFx = new InkEffect[total]; }

        int k = 0;
        foreach (TextRun r in _node.Runs)
        {
            foreach (char ch in r.Text)
            {
                if (k >= total) { break; }
                _chars[k] = ch; _cellFx[k] = r.Effect; k++;
            }
        }
        _cellCount = total;
    }

    // Re-place the speaker/body/choice columns to start at bodyLeft (shifts right of a portrait).
    private void RelayoutText(float bodyLeft)
    {
        Ui.SetRect(_speaker, bodyLeft, Pad, 600f, SpeakerH);
        for (int i = 0; i < MaxChoices; i++)
        {
            Ui.SetOffsets(_choiceUi[i], new Vector2(bodyLeft, ChoiceTop + i * ChoiceH), new Vector2(-Pad, ChoiceTop + (i + 1) * ChoiceH));
        }
    }

    public override void OnUpdate(float dt)
    {
        if (!Active) return;
        float udt = UnscaledDelta(Time.UnscaledTime);

        // Keep the ink materials breathing on the unscaled clock (panel grain + speaker glitch),
        // even during the open/close slides.
        Vector4 mt = new(Time.UnscaledTime, 0f, 0f, 0f);
        Ui.SetMaterialParams(_panel, mt);
        Ui.SetMaterialParams(_speaker, mt);

        // Box close slide owns the frame: animate down, swallow input, then finish.
        if (_closing)
        {
            _boxT = Math.Max(0f, _boxT - udt / CloseDur);
            ApplyBoxSlide(_boxT);
            if (_boxT <= 0f) { FinishClose(); }
            return;
        }

        // Open slide + per-node content fade-in.
        _boxT = Math.Min(1f, _boxT + udt / OpenDur);
        ApplyBoxSlide(_boxT);
        _nodeT = Math.Min(1f, _nodeT + udt / NodeFadeDur);
        ApplyNodeFade(_nodeT);

        if (_node == null) return;
        float strength = 0.6f + 0.4f * GameSettings.InkGlow;

        if (Input.IsKeyPressed(Key.Escape)) { RequestClose(); return; }

        bool tap = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.Enter)
                   || Input.IsMousePressed(MouseButton.Left);

        // Phase 1 - typewriter. A tap snaps the line to full (and dries every glyph at once). On the
        // frame the line completes we reveal choices (if any) and return, so the same tap never also
        // advances/activates.
        if (!_fullShown)
        {
            _reveal += udt * CharsPerSec;
            int shown = Math.Min(_cellCount, (int)_reveal);
            if (tap) { _reveal = _cellCount + BirthChars; shown = _cellCount; }
            RenderCells(shown, strength);
            if (shown >= _cellCount)
            {
                _fullShown = true;
                if (_node.HasChoices) { ShowChoices(); }
            }
            return;
        }

        // Phase 2 - fully shown; keep advancing the reveal clock so the final letters finish drying,
        // and keep the per-glyph effects animating.
        _reveal += udt * CharsPerSec;
        RenderCells(_cellCount, strength);

        if (_choicesShown) { UpdateChoices(); return; }

        if (tap) { GoTo(_node.Goto); } // linear advance
    }

    // Lay the revealed glyphs out on a monospace word-wrap grid and draw each as its own cell, passing a
    // per-glyph "birth" (0 = fresh wet blob, 1 = dried crisp letter) to the ink material. Recomputed each
    // frame so it self-corrects once the panel rect resolves and the per-glyph effects stay live.
    private void RenderCells(int shown, float strength)
    {
        float cellW = BodyFont * MonoAdvance;
        float bodyTop = Pad + SpeakerH + 6f;
        float panelW = Ui.GetRect(_panel).Z;
        float availW = panelW - _bodyLeft - Pad;
        int maxCols = availW > cellW ? Math.Max(1, (int)(availW / cellW)) : 9999;

        int n = _cellCount;
        int col = 0, row = 0, i = 0;
        while (i < n)
        {
            // Measure the next word (run of non-spaces) and wrap it whole if it fits on a line but not
            // in the remaining columns.
            int j = i;
            while (j < n && _chars[j] != ' ') { j++; }
            int wordLen = j - i;
            if (col > 0 && wordLen <= maxCols && col + wordLen > maxCols) { col = 0; row++; }

            for (int p = i; p < j; p++)
            {
                PlaceCell(p, col, row, shown, cellW, bodyTop, strength);
                col++;
                if (col >= maxCols) { col = 0; row++; }
            }

            // Spaces: consume them, keep their (glyph-less) cells hidden, and advance the column while
            // collapsing runs of spaces at the start of a wrapped line.
            while (j < n && _chars[j] == ' ')
            {
                if (_cellUi[j].IsValid) { _cellUi[j].SetActive(false); }
                if (col > 0) { col++; if (col >= maxCols) { col = 0; row++; } }
                j++;
            }
            i = j;
        }
    }

    private void PlaceCell(int i, int col, int row, int shown, float cellW, float bodyTop, float strength)
    {
        Entity e = _cellUi[i];
        if (!e.IsValid) { return; }
        if (i >= shown) { e.SetActive(false); return; }
        e.SetActive(true);
        float x = _bodyLeft + col * cellW;
        float y = bodyTop + row * LineH;
        Ui.SetOffsets(e, new Vector2(x, y), new Vector2(x + cellW + 2f, y + LineH));
        Ui.SetText(e, _chars[i].ToString());
        float birth = Math.Clamp((_reveal - i) / BirthChars, 0f, 1f);
        Ui.SetMaterialParams(e, new Vector4(Time.UnscaledTime, (float)_cellFx[i], strength, birth));
    }

    private void HideCells()
    {
        _cellCount = 0;
        for (int i = 0; i < MaxCells; i++)
        {
            if (_cellUi[i].IsValid) { _cellUi[i].SetActive(false); }
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
        _choiceStartU = Time.UnscaledTime;
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
        float ct = Time.UnscaledTime - _choiceStartU;
        for (int i = 0; i < n; i++)
        {
            // Stagger: each choice fades + slides up shortly after the previous.
            float reveal = Math.Clamp((ct - i * ChoiceStagger) / ChoiceFadeDur, 0f, 1f);
            float slide = (1f - reveal) * 10f;
            Ui.SetOffsets(_choiceUi[i], new Vector2(_bodyLeft, ChoiceTop + i * ChoiceH + slide),
                          new Vector2(-Pad, ChoiceTop + (i + 1) * ChoiceH + slide));

            bool on = i == _focus;
            Ui.SetText(_choiceUi[i], (on ? "> " : "  ") + _visible[i].Text);
            Vector4 col = on ? new Vector4(Accent.X, Accent.Y, Accent.Z, breathe * reveal)
                             : new Vector4(ChoiceDim.X, ChoiceDim.Y, ChoiceDim.Z, ChoiceDim.W * reveal);
            Ui.SetTextColor(_choiceUi[i], col);
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

    // Begin the close slide (world stays frozen until it finishes, so the closing key never leaks
    // into gameplay). FinishClose does the actual teardown once the box is tucked away.
    private void RequestClose()
    {
        if (_closing) return;
        _closing = true;
        _node = null;
        HideChoices();
    }

    private void FinishClose()
    {
        Active = false;
        _closing = false;
        _graph = null;
        _node = null;
        Dialogue.CurrentNodeId = null;
        Hide();
        Time.Resume();
    }

    private static float EaseOut(float x) => 1f - (1f - x) * (1f - x);

    // Slide the whole box (children ride along, being parented to the panel): tucked BoxH+ below the
    // bottom at t=0, eased into its seated offsets at t=1.
    private void ApplyBoxSlide(float t)
    {
        float yShift = (1f - EaseOut(Math.Clamp(t, 0f, 1f))) * (BoxH + Margin + 40f);
        Ui.SetOffsets(_panel, new Vector2(Margin, -(Margin + BoxH) + yShift), new Vector2(-Margin, -Margin + yShift));
    }

    // Fade the node's speaker + portrait in on a node change (the body already types on, so it needs
    // no separate fade).
    private void ApplyNodeFade(float t)
    {
        float a = Math.Clamp(t, 0f, 1f);
        Vector4 sc = Accent; sc.W = a;
        Ui.SetTextColor(_speaker, sc);
        if (_portrait.IsValid) { Ui.SetImageColor(_portrait, new Vector4(1f, 1f, 1f, a)); }
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
        _hint.SetActive(true);
    }

    private void Hide()
    {
        if (!_built) return;
        _panel.SetActive(false);
        _rule.SetActive(false);
        _portrait.SetActive(false);
        _speaker.SetActive(false);
        _hint.SetActive(false);
        HideChoices();
        HideCells();
        _lastU = -1f;
    }
}
