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

    private const string Dir = "project://assets/dialogue/";
    private static readonly string[] Effects = { "normal", "shake", "wave", "flicker", "whisper", "glitch" };
    private const float NodeW = 200f, NodeH = 66f, TitleH = 22f, SidePanelW = 320f;

    private static readonly Vector4 Bg = new(0.05f, 0.06f, 0.09f, 1f);
    private static readonly Vector4 NodeCol = new(0.10f, 0.12f, 0.16f, 1f);
    private static readonly Vector4 TitleCol = new(0.16f, 0.19f, 0.25f, 1f);
    private static readonly Vector4 Accent = new(0.30f, 0.85f, 1f, 1f);
    private static readonly Vector4 White = new(0.90f, 0.93f, 0.96f, 1f);
    private static readonly Vector4 Dim = new(0.58f, 0.64f, 0.70f, 1f);
    private static readonly Vector4 Link = new(0.45f, 0.55f, 0.62f, 1f);
    private static readonly Vector4 Current = new(1f, 0.78f, 0.30f, 1f);

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

        if (_files.Length == 0) { RefreshFiles(); }
        if (!_autoLoaded && _files.Length > 0) { _autoLoaded = true; Load(_files[0]); }

        // Toolbar.
        if (_files.Length > 0 && EditorGui.Combo("file", ref _fileIdx, _files)) { Load(_files[_fileIdx]); }
        EditorGui.SameLine();
        if (EditorGui.Button("Reload") && _loadedId.Length > 0) { Load(_loadedId); }
        EditorGui.SameLine();
        if (EditorGui.Button("Save") && _graph != null) { Save(); }
        EditorGui.SameLine();
        if (EditorGui.Button("Add Node") && _graph != null) { AddNode(); }
        EditorGui.Separator();

        if (_graph == null) { EditorGui.Text("Pick a conversation to edit."); EditorGui.End(); return; }

        float canvasW = EditorGui.ContentAvail().X - SidePanelW;
        if (canvasW < 200f) { canvasW = 200f; }

        DrawCanvas(canvasW);
        EditorGui.SameLine();
        DrawSidePanel();

        EditorGui.End();
    }

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
            n.Pos = new Vector2(40f + d * 250f, 40f + row * 96f);
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

    // ── Canvas ────────────────────────────────────────────────────────────────
    private void DrawCanvas(float width)
    {
        EditorGui.BeginChild("canvas", new Vector2(width, 0f), true);
        Vector2 origin = EditorGui.CursorScreenPos();
        Vector2 size = EditorGui.ContentAvail();
        EditorGui.AddRectFilled(origin, origin + size, Bg, 0f);

        HandleInput(origin, size);

        foreach (EdNode n in _graph!.Nodes) { DrawLinks(n, origin); }
        foreach (EdNode n in _graph.Nodes) { DrawNode(n, origin); }

        EditorGui.EndChild();
    }

    private void HandleInput(Vector2 origin, Vector2 size)
    {
        Vector2 mouse = EditorGui.MousePos();
        bool inCanvas = mouse.X >= origin.X && mouse.X <= origin.X + size.X && mouse.Y >= origin.Y && mouse.Y <= origin.Y + size.Y;

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

    private void DrawNode(EdNode n, Vector2 origin)
    {
        Vector2 s = NodeScreen(n, origin);
        Vector2 e = s + new Vector2(NodeW, NodeH);
        bool isStart = n.Id == _graph!.Start;
        bool isCurrent = Dialogue.IsActive && n.Id == Dialogue.CurrentNodeId;

        EditorGui.AddRectFilled(s, e, NodeCol, 5f);
        EditorGui.AddRectFilled(s, new Vector2(e.X, s.Y + TitleH), isStart ? new Vector4(0.10f, 0.28f, 0.34f, 1f) : TitleCol, 5f);
        if (isCurrent) { EditorGui.AddRect(s, e, Current, 5f, 3f); }
        else if (n == _selected) { EditorGui.AddRect(s, e, Accent, 5f, 2f); }

        EditorGui.AddText(s + new Vector2(7f, 3f), isStart ? Accent : White, isStart ? n.Id + "  *" : n.Id);
        EditorGui.AddText(s + new Vector2(7f, TitleH + 5f), Dim, Trunc($"{n.Speaker}: {n.Text}", 28));
        EditorGui.AddText(s + new Vector2(7f, TitleH + 24f), Dim, n.HasChoices ? $"{n.Choices.Count} choices" : (n.Goto.Length > 0 ? $"-> {n.Goto}" : "end"));
    }

    private void DrawLinks(EdNode n, Vector2 origin)
    {
        Vector2 s = NodeScreen(n, origin);
        if (n.HasChoices)
        {
            int count = n.Choices.Count;
            for (int i = 0; i < count; i++)
            {
                Vector2 outP = s + new Vector2(NodeW * (i + 1f) / (count + 1f), NodeH);
                LinkTo(outP, n.Choices[i].Goto, origin);
            }
        }
        else if (n.Goto.Length > 0)
        {
            LinkTo(s + new Vector2(NodeW * 0.5f, NodeH), n.Goto, origin);
        }
    }

    private void LinkTo(Vector2 outP, string targetId, Vector2 origin)
    {
        EdNode? t = _graph!.Find(targetId);
        if (t == null) { return; }
        Vector2 inP = NodeScreen(t, origin) + new Vector2(NodeW * 0.5f, 0f);
        EditorGui.AddBezierCubic(outP, outP + new Vector2(0f, 52f), inP - new Vector2(0f, 52f), inP, Link, 2f);
        EditorGui.AddCircleFilled(outP, 3f, Accent);
    }

    // ── Side panel (edit the selected node) ───────────────────────────────────
    private void DrawSidePanel()
    {
        EditorGui.BeginChild("side", new Vector2(SidePanelW - 8f, 0f), true);
        if (_selected == null) { EditorGui.Text("Select a node to edit."); EditorGui.EndChild(); return; }
        EdNode n = _selected;

        EditorGui.Text($"node: {n.Id}");
        EditorGui.InputText("speaker", ref n.Speaker);
        EditorGui.InputText("text", ref n.Text, 512);
        EditorGui.InputText("portrait", ref n.Portrait);

        int eff = System.Array.IndexOf(Effects, n.Effect);
        if (eff < 0) { eff = 0; }
        if (EditorGui.Combo("effect", ref eff, Effects)) { n.Effect = Effects[eff]; }

        EditorGui.Separator();
        if (!n.HasChoices)
        {
            EditorGui.InputText("goto", ref n.Goto);
        }
        EditorGui.Text("choices:");
        for (int i = 0; i < n.Choices.Count; i++)
        {
            EdChoice c = n.Choices[i];
            EditorGui.InputText($"text##{i}", ref c.Text);
            EditorGui.InputText($"goto##{i}", ref c.Goto);
            EditorGui.InputText($"if##{i}", ref c.If);
            EditorGui.InputText($"set##{i}", ref c.Set);
            if (EditorGui.SmallButton($"delete choice##{i}")) { n.Choices.RemoveAt(i); break; }
            EditorGui.Separator();
        }
        if (EditorGui.Button("+ choice")) { n.Choices.Add(new EdChoice { Text = "...", Goto = "end" }); }

        EditorGui.Separator();
        if (EditorGui.Button("set as start")) { _graph!.Start = n.Id; }
        EditorGui.SameLine();
        if (EditorGui.Button("delete node")) { DeleteNode(n); }

        EditorGui.EndChild();
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

    private static string Trunc(string s, int max) => s.Length <= max ? s : s[..(max - 1)] + "~";
}
