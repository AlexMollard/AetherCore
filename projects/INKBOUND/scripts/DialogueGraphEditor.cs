using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Text;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>Visual node-graph editor for INKBOUND dialogue. An IEditorWindow drawn with EditorGui:
/// pick a conversation, arrange nodes on a canvas, edit every field + choices, add/delete nodes, and
/// save back to the runtime JSON. Node positions live in a sidecar &lt;id&gt;.layout.json so the data
/// file stays clean. Canvas interaction is C#-side (raw mouse queries + hit-testing) over the draw list.</summary>
public sealed class DialogueGraphEditor : IEditorWindow
{
    public string Title => "Dialogue Graph";

    // Backed by _open so the editor's Project menu can show/hide this window, and the window's own X
    // button (which clears _open) keeps that menu toggle in sync.
    public bool Visible { get => _open; set => _open = value; }

    private const string Dir = "project://assets/dialogue/";
    private static readonly string[] Effects = { "normal", "shake", "wave", "flicker", "whisper", "glitch" };
    private const float NodeW = 236f, NodeH = 122f, TitleH = 28f, SidePanelW = 330f, GridStep = 32f;

    // Font Awesome 6 glyphs baked into the editor font (0xE000-0xF8FF; solid+regular). Same set the
    // editor menus use, so these render rather than tofu.
    private static class Ico
    {
        public const string Save = "\uf0c7";       // floppy-disk
        public const string Reload = "\uf021";     // arrows-rotate
        public const string Plus = "\uf055";       // circle-plus
        public const string Comment = "\uf075";    // comment
        public const string Branch = "\uf126";     // code-branch
        public const string Play = "\uf04b";       // play
        public const string Dot = "\uf111";        // circle
        public const string User = "\uf007";       // user
        public const string Font = "\uf031";       // font
        public const string Image = "\uf03e";      // image
        public const string Wand = "\ue2ca";       // wand-magic-sparkles
        public const string ArrowRight = "\uf061"; // arrow-right
        public const string Flag = "\uf024";       // flag
        public const string Trash = "\uf1f8";      // trash
        public const string ChevUp = "\uf077";     // chevron-up
        public const string ChevDown = "\uf078";   // chevron-down
    }
    private static readonly Vector4 Current = new(1f, 0.78f, 0.30f, 1f); // play-mode "current node" (fixed warm)

    // Theme colours, refreshed each frame from the editor's live ImGui style (see RefreshTheme).
    private Vector4 _cBg, _cNode, _cNodeHover, _cHeader, _cText, _cDim, _cBorder, _cAccent, _cLink;
    private EdNode? _hover;

    private bool _open = true;
    private bool _autoLoaded;
    private string[] _files = System.Array.Empty<string>();
    private int _fileIdx = -1;
    private EdGraph? _graph;
    private string _loadedId = "";
    private Vector2 _pan;
    private EdNode? _selected;
    private int _dragMode; // 0 none, 1 node, 2 pan
    private EdNode? _dragNode;
    private Vector2 _dragStart;

    public void OnGui()
    {
        EditorGui.SetNextWindowSize(new Vector2(1180f, 720f));
        if (!EditorGui.Begin(Title, ref _open)) { EditorGui.End(); return; }

        RefreshTheme();
        if (_files.Length == 0) { RefreshFiles(); }
        if (!_autoLoaded && _files.Length > 0) { _autoLoaded = true; Load(_files[0]); }

        DrawToolbar();

        if (_graph == null) { EditorGui.Text("Pick a conversation to edit."); EditorGui.End(); return; }

        float canvasW = EditorGui.ContentAvail().X - SidePanelW;
        if (canvasW < 200f) { canvasW = 200f; }

        DrawCanvas(canvasW);
        EditorGui.SameLine();
        DrawSidePanel();

        EditorGui.End();
    }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    // A themed bar: accent title chip, conversation picker, custom-drawn action buttons with hover
    // states, and a right-aligned node/status badge. Drawn on the window draw list so it follows the
    // editor's live theme like the rest of the graph.
    private void DrawToolbar()
    {
        Vector2 p0 = EditorGui.CursorScreenPos();
        float w = EditorGui.ContentAvail().X;
        float frameH = EditorGui.FrameHeight();      // real combo/button height (font + frame padding)
        float barH = frameH + 12f;                    // 6px breathing room above and below the controls
        float midY = p0.Y + barH * 0.5f;
        float lineH = EditorGui.CalcTextSize("Xg").Y;
        float textY = midY - lineH * 0.5f;            // centre plain text on the bar

        var barBg = new Vector4(_cNode.X, _cNode.Y, _cNode.Z, 1f);
        EditorGui.AddRectFilled(p0, new Vector2(p0.X + w, p0.Y + barH), barBg, 5f);
        EditorGui.AddRect(p0, new Vector2(p0.X + w, p0.Y + barH), _cBorder, 5f, 1f);
        EditorGui.AddLine(new Vector2(p0.X + 4f, p0.Y + barH), new Vector2(p0.X + w - 4f, p0.Y + barH),
            new Vector4(_cAccent.X, _cAccent.Y, _cAccent.Z, 0.45f), 1.5f);

        // Accent chip + loaded conversation id.
        Vector2 chip = new(p0.X + 12f, midY - 7f);
        EditorGui.AddRectFilled(chip, chip + new Vector2(5f, 14f), _cAccent, 1.5f);
        string id = _loadedId.Length > 0 ? _loadedId.ToUpperInvariant() : "DIALOGUE";
        string title = $"{Ico.Comment}  {id}";
        EditorGui.AddText(new Vector2(p0.X + 24f, textY), _cAccent, title);
        float titleW = EditorGui.CalcTextSize(title).X;

        // Controls row: centre the frame-height widgets in the bar; combo width fixed so buttons follow.
        float rowY = midY - frameH * 0.5f;
        EditorGui.SetCursorScreenPos(new Vector2(p0.X + 24f + titleW + 16f, rowY));
        EditorGui.SetNextItemWidth(150f);
        if (_files.Length > 0 && EditorGui.Combo("##file", ref _fileIdx, _files)) { Load(_files[_fileIdx]); }
        EditorGui.SameLine();
        if (ToolButton("##reload", $"{Ico.Reload}  Reload", false, frameH)) { RefreshFiles(); if (_loadedId.Length > 0) { Load(_loadedId); } }
        EditorGui.SameLine();
        if (ToolButton("##save", $"{Ico.Save}  Save", true, frameH) && _graph != null) { Save(); }
        EditorGui.SameLine();
        if (ToolButton("##addnode", $"{Ico.Plus}  Node", true, frameH) && _graph != null) { AddNode(); }

        // Right-aligned status/hint.
        if (_graph != null)
        {
            string status = $"{_graph.Nodes.Count} nodes   drag to pan, node to move";
            float sw = EditorGui.CalcTextSize(status).X;
            EditorGui.AddText(new Vector2(p0.X + w - sw - 12f, textY), _cDim, status);
        }

        EditorGui.SetCursorScreenPos(new Vector2(p0.X, p0.Y + barH + 6f));
    }

    // A rounded, hover-lit toolbar button drawn on the draw list, sized to the given height so it lines
    // up with the combo. Accent = filled amber with dark text (primary actions); else a bordered chip.
    private bool ToolButton(string id, string label, bool accent, float h)
    {
        Vector2 ts = EditorGui.CalcTextSize(label);
        const float padX = 11f;
        var size = new Vector2(ts.X + padX * 2f, h);
        Vector2 p = EditorGui.CursorScreenPos();
        bool clicked = EditorGui.InvisibleButton(id, size);
        bool hover = EditorGui.IsItemHovered();
        Vector4 bg = accent ? (hover ? Lighten(_cAccent, 0.15f) : _cAccent)
                            : (hover ? _cNodeHover : _cNode);
        Vector4 fg = accent ? _cBg : _cText;
        EditorGui.AddRectFilled(p, p + size, bg, 5f);
        if (!accent) { EditorGui.AddRect(p, p + size, _cBorder, 5f, 1f); }
        EditorGui.AddText(new Vector2(p.X + padX, p.Y + (h - ts.Y) * 0.5f), fg, label);
        return clicked;
    }

    private static Vector4 Lighten(Vector4 c, float t) =>
        new(c.X + (1f - c.X) * t, c.Y + (1f - c.Y) * t, c.Z + (1f - c.Z) * t, c.W);

    // ── Files / load / save ───────────────────────────────────────────────────
    private void RefreshFiles()
    {
        var ids = new List<string>();
        foreach (string p in Assets.List(Dir + "*.json"))
        {
            string name = Path.GetFileName(p);
            if (name.EndsWith(".layout.json")) { continue; }
            ids.Add(name.EndsWith(".json") ? name[..^5] : name);
        }
        ids.Sort(System.StringComparer.Ordinal);
        _files = ids.ToArray();
    }

    private void Load(string id)
    {
        string? json = Assets.ReadText($"{Dir}{id}.json");
        if (json == null) { Log.Warn($"[INKBOUND] graph editor: '{id}.json' not found"); return; }
        _graph = EdGraph.Parse(json);
        _loadedId = id;
        _selected = null;
        _pan = default;
        _fileIdx = System.Array.IndexOf(_files, id);
        if (_graph == null) { return; }
        LoadLayout(id);
    }

    private void LoadLayout(string id)
    {
        string? layout = Assets.ReadText($"{Dir}{id}.layout.json");
        bool any = false;
        if (layout != null)
        {
            try
            {
                using var doc = JsonDocument.Parse(layout);
                JsonElement root = doc.RootElement;
                if (root.TryGetProperty("pan", out JsonElement pan) && pan.ValueKind == JsonValueKind.Array)
                {
                    _pan = new Vector2((float)pan[0].GetDouble(), (float)pan[1].GetDouble());
                }
                if (root.TryGetProperty("positions", out JsonElement pos) && pos.ValueKind == JsonValueKind.Object)
                {
                    foreach (JsonProperty p in pos.EnumerateObject())
                    {
                        EdNode? n = _graph!.Find(p.Name);
                        if (n != null && p.Value.ValueKind == JsonValueKind.Array)
                        {
                            n.Pos = new Vector2((float)p.Value[0].GetDouble(), (float)p.Value[1].GetDouble());
                            any = true;
                        }
                    }
                }
            }
            catch (JsonException) { }
        }
        if (!any) { AutoLayout(); }
    }

    private void AutoLayout()
    {
        // Layered BFS from Start: x by depth, y stacked within a depth.
        var depth = new Dictionary<string, int>();
        var queue = new Queue<string>();
        if (_graph!.Find(_graph.Start) != null) { depth[_graph.Start] = 0; queue.Enqueue(_graph.Start); }
        while (queue.Count > 0)
        {
            string id = queue.Dequeue();
            EdNode? n = _graph.Find(id);
            if (n == null) { continue; }
            foreach (string t in Targets(n))
            {
                if (_graph.Find(t) != null && !depth.ContainsKey(t)) { depth[t] = depth[id] + 1; queue.Enqueue(t); }
            }
        }
        var rowInDepth = new Dictionary<int, int>();
        foreach (EdNode n in _graph.Nodes)
        {
            int d = depth.TryGetValue(n.Id, out int dv) ? dv : 0;
            int row = rowInDepth.TryGetValue(d, out int rv) ? rv : 0;
            rowInDepth[d] = row + 1;
            n.Pos = new Vector2(48f + d * 300f, 48f + row * 152f);
        }
    }

    private static IEnumerable<string> Targets(EdNode n)
    {
        if (n.HasChoices) { foreach (EdChoice c in n.Choices) { if (c.Goto.Length > 0) { yield return c.Goto; } } }
        else if (n.Goto.Length > 0) { yield return n.Goto; }
    }

    private void Save()
    {
        Assets.WriteText($"{Dir}{_loadedId}.json", _graph!.ToJson());
        Assets.WriteText($"{Dir}{_loadedId}.layout.json", LayoutJson());
        Log.Info($"[INKBOUND] Saved dialogue '{_loadedId}' ({_graph.Nodes.Count} nodes).");
    }

    private string LayoutJson()
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteStartArray("pan");
            w.WriteNumberValue(_pan.X); w.WriteNumberValue(_pan.Y);
            w.WriteEndArray();
            w.WriteStartObject("positions");
            foreach (EdNode n in _graph!.Nodes)
            {
                w.WriteStartArray(n.Id);
                w.WriteNumberValue(n.Pos.X); w.WriteNumberValue(n.Pos.Y);
                w.WriteEndArray();
            }
            w.WriteEndObject();
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    // ── Theme ─────────────────────────────────────────────────────────────────
    private void RefreshTheme()
    {
        Vector4 win = EditorGui.ThemeColor(EditorColor.WindowBg);
        _cBg = new Vector4(win.X * 0.55f, win.Y * 0.55f, win.Z * 0.55f, 1f); // canvas: a touch darker for depth
        _cNode = EditorGui.ThemeColor(EditorColor.PanelBg);
        _cNodeHover = EditorGui.ThemeColor(EditorColor.PanelHover);
        _cHeader = EditorGui.ThemeColor(EditorColor.Header);
        _cText = EditorGui.ThemeColor(EditorColor.Text);
        _cDim = EditorGui.ThemeColor(EditorColor.TextDim);
        _cBorder = EditorGui.ThemeColor(EditorColor.Border);
        _cAccent = EditorGui.ThemeColor(EditorColor.Accent);
        _cLink = EditorGui.ThemeColor(EditorColor.Link);
        if (_cLink.W < 0.2f) { _cLink = _cDim; } // some themes leave PlotLines faint
    }

    // ── Canvas ────────────────────────────────────────────────────────────────
    private void DrawCanvas(float width)
    {
        EditorGui.BeginChild("canvas", new Vector2(width, 0f), true);
        Vector2 origin = EditorGui.CursorScreenPos();
        Vector2 size = EditorGui.ContentAvail();
        EditorGui.AddRectFilled(origin, origin + size, _cBg, 0f);
        DrawGrid(origin, size);

        HandleInput(origin, size);

        foreach (EdNode n in _graph!.Nodes) { DrawLinks(n, origin); }
        foreach (EdNode n in _graph.Nodes) { DrawNode(n, origin); }

        EditorGui.EndChild();
    }

    private void DrawGrid(Vector2 origin, Vector2 size)
    {
        var line = new Vector4(_cBorder.X, _cBorder.Y, _cBorder.Z, 0.22f);
        float ox = ((_pan.X % GridStep) + GridStep) % GridStep;
        float oy = ((_pan.Y % GridStep) + GridStep) % GridStep;
        for (float x = ox; x < size.X; x += GridStep)
        {
            EditorGui.AddLine(new Vector2(origin.X + x, origin.Y), new Vector2(origin.X + x, origin.Y + size.Y), line, 1f);
        }
        for (float y = oy; y < size.Y; y += GridStep)
        {
            EditorGui.AddLine(new Vector2(origin.X, origin.Y + y), new Vector2(origin.X + size.X, origin.Y + y), line, 1f);
        }
    }

    private void HandleInput(Vector2 origin, Vector2 size)
    {
        Vector2 mouse = EditorGui.MousePos();
        bool inCanvas = mouse.X >= origin.X && mouse.X <= origin.X + size.X && mouse.Y >= origin.Y && mouse.Y <= origin.Y + size.Y;
        _hover = inCanvas && _dragMode == 0 ? TopNodeAt(mouse, origin) : null;

        if (EditorGui.IsMouseClicked() && inCanvas)
        {
            EdNode? hit = TopNodeAt(mouse, origin);
            if (hit != null) { _selected = hit; _dragMode = 1; _dragNode = hit; _dragStart = hit.Pos; }
            else { _selected = null; _dragMode = 2; _dragStart = _pan; }
        }

        if (_dragMode != 0)
        {
            if (EditorGui.IsMouseDown())
            {
                Vector2 d = EditorGui.MouseDragDelta();
                if (_dragMode == 1 && _dragNode != null) { _dragNode.Pos = _dragStart + d; }
                else if (_dragMode == 2) { _pan = _dragStart + d; }
            }
            else { _dragMode = 0; _dragNode = null; }
        }
    }

    private EdNode? TopNodeAt(Vector2 mouse, Vector2 origin)
    {
        for (int i = _graph!.Nodes.Count - 1; i >= 0; i--)
        {
            EdNode n = _graph.Nodes[i];
            Vector2 s = NodeScreen(n, origin);
            if (mouse.X >= s.X && mouse.X <= s.X + NodeW && mouse.Y >= s.Y && mouse.Y <= s.Y + NodeH) { return n; }
        }
        return null;
    }

    private Vector2 NodeScreen(EdNode n, Vector2 origin) => origin + _pan + n.Pos;

    private static int OutCount(EdNode n) => n.HasChoices ? n.Choices.Count : (n.Goto.Length > 0 ? 1 : 0);

    private Vector2 OutPort(EdNode n, int i, int count, Vector2 s)
        => s + new Vector2(count <= 1 ? NodeW * 0.5f : NodeW * (i + 1f) / (count + 1f), NodeH);

    private void DrawNode(EdNode n, Vector2 origin)
    {
        Vector2 s = NodeScreen(n, origin);
        Vector2 e = s + new Vector2(NodeW, NodeH);
        bool isStart = n.Id == _graph!.Start;
        bool isCurrent = Dialogue.IsActive && n.Id == Dialogue.CurrentNodeId;
        bool isSel = n == _selected;
        bool isHover = n == _hover;
        bool isEnd = !n.HasChoices && n.Goto.Length == 0;

        // Type accent: start = accent, branch = accent-dim, end = faint red-ish (from dim), linear = border.
        Vector4 typeCol = isStart ? _cAccent : n.HasChoices ? _cAccent : isEnd ? new Vector4(0.7f, 0.35f, 0.3f, 1f) : _cBorder;

        // Shadow, body, header, left type stripe.
        EditorGui.AddRectFilled(s + new Vector2(4f, 5f), e + new Vector2(4f, 5f), new Vector4(0f, 0f, 0f, 0.40f), 9f);
        EditorGui.AddRectFilled(s, e, isHover ? _cNodeHover : _cNode, 9f);
        Vector4 head = isStart ? _cAccent : _cHeader;
        EditorGui.AddRectFilled(s, new Vector2(e.X, s.Y + TitleH), head, 9f);
        EditorGui.AddRectFilled(new Vector2(s.X, s.Y + TitleH - 9f), new Vector2(e.X, s.Y + TitleH), head, 0f);
        EditorGui.AddRectFilled(s, new Vector2(s.X + 4f, e.Y), typeCol, 0f); // type stripe
        Vector4 border = isCurrent ? Current : isSel ? _cAccent : _cBorder;
        EditorGui.AddRect(s, e, border, 9f, (isSel || isCurrent) ? 2.5f : 1f);

        // Clip all inner text/pills to the card interior so nothing spills past the border.
        float inX = s.X + 12f, innerW = NodeW - 22f;
        EditorGui.PushClipRect(new Vector2(s.X + 5f, s.Y), new Vector2(e.X - 3f, e.Y), true);

        // Header: id + a right-aligned type tag.
        Vector4 headText = isStart ? EditorGui.ThemeColor(EditorColor.WindowBg) : _cText;
        EditorGui.AddText(new Vector2(inX, s.Y + 6f), headText, Fit(n.Id, innerW - 52f));
        string tag = isStart ? "START" : n.HasChoices ? "BRANCH" : isEnd ? "END" : "LINE";
        string tagIco = isStart ? Ico.Play : n.HasChoices ? Ico.Branch : isEnd ? Ico.Dot : Ico.ArrowRight;
        string tagStr = $"{tagIco} {tag}";
        Vector4 tagCol = new(headText.X, headText.Y, headText.Z, 0.65f);
        EditorGui.AddText(new Vector2(e.X - 10f - EditorGui.CalcTextSize(tagStr).X, s.Y + 7f), tagCol, tagStr);

        // Body: speaker + text preview, with breathing room between rows.
        EditorGui.AddText(new Vector2(inX, s.Y + TitleH + 13f), _cAccent, Fit(n.Speaker.Length > 0 ? n.Speaker : "(no speaker)", innerW));
        EditorGui.AddText(new Vector2(inX, s.Y + TitleH + 37f), _cText, Fit(n.Text.Length > 0 ? n.Text : "...", innerW));

        // Footer pills: effect + destination. Dark chip + bright text so labels stay legible on the card.
        float fy = e.Y - 28f;
        float fx = inX;
        if (n.Effect != "normal") { fx = Pill(fx, fy, $"{Ico.Wand} {n.Effect}", PillBg(), _cAccent); }
        string dest = n.HasChoices ? $"{Ico.Branch} {n.Choices.Count} choices" : n.Goto.Length > 0 ? $"{Ico.ArrowRight} {n.Goto}" : $"{Ico.Dot} end";
        Pill(fx, fy, dest, PillBg(), _cText);

        EditorGui.PopClipRect();

        // Ports: input (top-centre) + one output per destination (bottom).
        EditorGui.AddCircleFilled(new Vector2(s.X + NodeW * 0.5f, s.Y), 4f, _cBorder);
        int outs = OutCount(n);
        for (int i = 0; i < outs; i++) { EditorGui.AddCircleFilled(OutPort(n, i, outs, s), 4f, _cAccent); }
    }

    // Dark chip (the darker canvas colour) so bright pill text reads clearly against the node body.
    private Vector4 PillBg() => new(_cBg.X, _cBg.Y, _cBg.Z, 0.92f);

    private float Pill(float x, float y, string label, Vector4 bg, Vector4 fg)
    {
        // Size the chip to the text (icon glyphs are taller than a fixed 18px, so derive the height).
        Vector2 sz = EditorGui.CalcTextSize(label);
        const float padX = 6f, padY = 3f;
        float h = sz.Y + padY * 2f;
        var a = new Vector2(x, y);
        var b = new Vector2(x + sz.X + padX * 2f, y + h);
        EditorGui.AddRectFilled(a, b, bg, 4f);
        EditorGui.AddRect(a, b, new Vector4(fg.X, fg.Y, fg.Z, 0.35f), 4f, 1f); // subtle tint border for definition
        EditorGui.AddText(new Vector2(x + padX, y + padY), fg, label);
        return b.X + 5f;
    }

    private void DrawLinks(EdNode n, Vector2 origin)
    {
        Vector2 s = NodeScreen(n, origin);
        int count = OutCount(n);
        for (int i = 0; i < count; i++)
        {
            string goTo = n.HasChoices ? n.Choices[i].Goto : n.Goto;
            LinkTo(OutPort(n, i, count, s), goTo, origin);
        }
    }

    private void LinkTo(Vector2 outP, string targetId, Vector2 origin)
    {
        EdNode? t = _graph!.Find(targetId);
        if (t == null) { return; }
        Vector2 inP = NodeScreen(t, origin) + new Vector2(NodeW * 0.5f, 0f);
        float bow = System.Math.Max(45f, System.Math.Abs(inP.Y - outP.Y) * 0.5f);
        EditorGui.AddBezierCubic(outP, outP + new Vector2(0f, bow), inP - new Vector2(0f, bow), inP, _cLink, 2.5f);
        EditorGui.AddTriangleFilled(inP + new Vector2(-5f, -9f), inP + new Vector2(5f, -9f), inP + new Vector2(0f, 1f), _cLink);
    }

    // ── Side panel (edit the selected node) ───────────────────────────────────
    private void DrawSidePanel()
    {
        EditorGui.BeginChild("side", new Vector2(SidePanelW - 8f, 0f), true);
        if (_selected == null)
        {
            EditorGui.Spacing();
            EditorGui.TextColored(_cDim, "No node selected.");
            EditorGui.TextColored(_cDim, "Click a node on the canvas to edit it,");
            EditorGui.TextColored(_cDim, "or press + Node to add one.");
            EditorGui.EndChild();
            return;
        }
        EdNode n = _selected;
        bool isEnd = !n.HasChoices && n.Goto.Length == 0;
        string type = n.Id == _graph!.Start ? "START" : n.HasChoices ? "BRANCH" : isEnd ? "END" : "LINE";

        // ── Node ──
        Section(Ico.Comment, "NODE");
        EditorGui.Text(n.Id);
        EditorGui.SameLine();
        EditorGui.TextColored(_cAccent, type);

        Field("speaker", Ico.User, "speaker", ref n.Speaker);

        EditorGui.TextColored(_cDim, $"{Ico.Font}  text");
        EditorGui.InputTextMultiline("##text", ref n.Text, new Vector2(-1f, 70f), 512);

        Field("portrait", Ico.Image, "portrait", ref n.Portrait);

        EditorGui.TextColored(_cDim, $"{Ico.Wand}  effect");
        int eff = System.Array.IndexOf(Effects, n.Effect);
        if (eff < 0) { eff = 0; }
        EditorGui.SetNextItemWidth(-1f);
        if (EditorGui.Combo("##effect", ref eff, Effects)) { n.Effect = Effects[eff]; }

        if (!n.HasChoices) { Field("goto", Ico.ArrowRight, "goto", ref n.Goto); }

        // ── Choices ──
        Section(Ico.Branch, "CHOICES");
        if (n.Choices.Count == 0)
        {
            EditorGui.TextColored(_cDim, "None yet - add one to branch");
            EditorGui.TextColored(_cDim, "(choices override goto).");
        }
        int del = -1;
        (int from, int to) move = (-1, -1);
        for (int i = 0; i < n.Choices.Count; i++)
        {
            EdChoice c = n.Choices[i];
            EditorGui.TextColored(_cAccent, $"Choice {i + 1}");
            EditorGui.SameLine();
            if (EditorGui.SmallButton($"{Ico.Trash}##ch{i}")) { del = i; }
            if (i > 0) { EditorGui.SameLine(); if (EditorGui.SmallButton($"{Ico.ChevUp}##up{i}")) { move = (i, i - 1); } }
            if (i < n.Choices.Count - 1) { EditorGui.SameLine(); if (EditorGui.SmallButton($"{Ico.ChevDown}##dn{i}")) { move = (i, i + 1); } }

            Field($"ctext{i}", Ico.Comment, "text", ref c.Text);
            Field($"cgoto{i}", Ico.ArrowRight, "goto", ref c.Goto);
            Field($"cif{i}", Ico.Branch, "show if", ref c.If);
            Field($"cset{i}", Ico.Flag, "on pick set", ref c.Set);
            EditorGui.Separator();
        }
        if (del >= 0) { n.Choices.RemoveAt(del); }
        else if (move.from >= 0)
        {
            EdChoice t = n.Choices[move.from];
            n.Choices.RemoveAt(move.from);
            n.Choices.Insert(move.to, t);
        }
        if (EditorGui.Button($"{Ico.Plus}  Choice", new Vector2(-1f, 0f))) { n.Choices.Add(new EdChoice { Text = "...", Goto = "end" }); }

        // ── Actions ──
        Section(Ico.Play, "ACTIONS");
        bool isStart = n.Id == _graph.Start;
        if (!isStart) { if (EditorGui.Button($"{Ico.Play}  Set as Start")) { _graph.Start = n.Id; } }
        else { EditorGui.TextColored(_cDim, "This is the start node."); }
        if (EditorGui.Button($"{Ico.Trash}  Delete Node", new Vector2(-1f, 0f))) { DeleteNode(n); }

        EditorGui.EndChild();
    }

    // A left-accent-barred section header, matching the toolbar's accent styling.
    private void Section(string icon, string label)
    {
        EditorGui.Spacing();
        Vector2 p = EditorGui.CursorScreenPos();
        EditorGui.AddRectFilled(new Vector2(p.X, p.Y + 2f), new Vector2(p.X + 3f, p.Y + 15f), _cAccent, 1f);
        EditorGui.SetCursorScreenPos(new Vector2(p.X + 9f, p.Y));
        EditorGui.TextColored(_cAccent, $"{icon}  {label}");
        EditorGui.Spacing();
    }

    // A dim icon+label above a full-width input. Returns true on change.
    private bool Field(string id, string icon, string label, ref string value, int maxLen = 256)
    {
        EditorGui.TextColored(_cDim, $"{icon}  {label}");
        EditorGui.SetNextItemWidth(-1f);
        return EditorGui.InputText("##" + id, ref value, maxLen);
    }

    // ── Structural ────────────────────────────────────────────────────────────
    private void AddNode()
    {
        string id = "node" + _graph!.Nodes.Count;
        while (_graph.Find(id) != null) { id += "_"; }
        var n = new EdNode { Id = id, Speaker = "The Void", Text = "...", Goto = "end", Pos = -_pan + new Vector2(120f, 120f) };
        _graph.Nodes.Add(n);
        _selected = n;
    }

    private void DeleteNode(EdNode n)
    {
        _graph!.Nodes.Remove(n);
        if (_selected == n) { _selected = null; }
        // Clear danglers pointing at it.
        foreach (EdNode m in _graph.Nodes)
        {
            if (m.Goto == n.Id) { m.Goto = ""; }
            foreach (EdChoice c in m.Choices) { if (c.Goto == n.Id) { c.Goto = "end"; } }
        }
    }

    // Trim to fit maxW pixels with an ellipsis (measured against the editor font).
    private static string Fit(string s, float maxW)
    {
        if (s.Length == 0 || EditorGui.CalcTextSize(s).X <= maxW) { return s; }
        while (s.Length > 1 && EditorGui.CalcTextSize(s + "...").X > maxW) { s = s[..^1]; }
        return s + "...";
    }
}
