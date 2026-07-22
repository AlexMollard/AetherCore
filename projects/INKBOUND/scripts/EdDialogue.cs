using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Text;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

// Editor-side MUTABLE dialogue model for the node-graph editor. Distinct from the runtime DialogueGraph
// (which tokenizes text into runs and is read-mostly): here Text stays RAW (inline [tag] markup intact)
// so it round-trips, and everything is editable. Pos is canvas position (sidecar-persisted, never in
// the dialogue JSON). Parse/ToJson preserve the exact runtime schema so the runner keeps parsing it.

public sealed class EdChoice
{
    public string Text = "";
    public string Goto = "";
    public string If = "";
    public string Set = "";
}

public sealed class EdNode
{
    public string Id = "";
    public string Speaker = "";
    public string Portrait = "";
    public string Text = "";
    public string Effect = "normal";
    public string Goto = "";
    public List<EdChoice> Choices = new();
    public Vector2 Pos;

    public bool HasChoices => Choices.Count > 0;
}

public sealed class EdGraph
{
    public string Id = "";
    public string Start = "";
    public List<EdNode> Nodes = new();

    public EdNode? Find(string id)
    {
        foreach (EdNode n in Nodes) { if (n.Id == id) { return n; } }
        return null;
    }

    public static EdGraph? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            var g = new EdGraph { Id = Str(root, "id"), Start = Str(root, "start") };
            if (root.TryGetProperty("nodes", out JsonElement nodes) && nodes.ValueKind == JsonValueKind.Object)
            {
                foreach (JsonProperty np in nodes.EnumerateObject())
                {
                    JsonElement ne = np.Value;
                    var node = new EdNode
                    {
                        Id = np.Name,
                        Speaker = Str(ne, "speaker"),
                        Portrait = Str(ne, "portrait"),
                        Text = Str(ne, "text"),
                        Effect = Str(ne, "effect") is { Length: > 0 } e ? e : "normal",
                        Goto = Str(ne, "goto"),
                    };
                    if (ne.TryGetProperty("choices", out JsonElement ch) && ch.ValueKind == JsonValueKind.Array)
                    {
                        foreach (JsonElement ce in ch.EnumerateArray())
                        {
                            node.Choices.Add(new EdChoice
                            {
                                Text = Str(ce, "text"),
                                Goto = Str(ce, "goto"),
                                If = Str(ce, "if"),
                                Set = Str(ce, "set"),
                            });
                        }
                    }
                    g.Nodes.Add(node);
                }
            }
            return g;
        }
        catch (JsonException ex)
        {
            Log.Warn($"[INKBOUND] EdGraph.Parse failed: {ex.Message}");
            return null;
        }
    }

    /// <summary>Serialize back to the runtime schema (indented; optional fields omitted; Start node
    /// first). Positions are NOT written here - they live in the sidecar.</summary>
    public string ToJson()
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            w.WriteStartObject();
            w.WriteString("id", Id);
            w.WriteString("start", Start);
            w.WriteStartObject("nodes");
            foreach (EdNode n in Ordered())
            {
                w.WriteStartObject(n.Id);
                w.WriteString("speaker", n.Speaker);
                if (!string.IsNullOrEmpty(n.Portrait)) { w.WriteString("portrait", n.Portrait); }
                w.WriteString("text", n.Text);
                if (!string.IsNullOrEmpty(n.Effect) && n.Effect != "normal") { w.WriteString("effect", n.Effect); }
                if (n.HasChoices)
                {
                    w.WriteStartArray("choices");
                    foreach (EdChoice c in n.Choices)
                    {
                        w.WriteStartObject();
                        w.WriteString("text", c.Text);
                        w.WriteString("goto", c.Goto);
                        if (!string.IsNullOrEmpty(c.If)) { w.WriteString("if", c.If); }
                        if (!string.IsNullOrEmpty(c.Set)) { w.WriteString("set", c.Set); }
                        w.WriteEndObject();
                    }
                    w.WriteEndArray();
                }
                else if (!string.IsNullOrEmpty(n.Goto))
                {
                    w.WriteString("goto", n.Goto);
                }
                w.WriteEndObject();
            }
            w.WriteEndObject();
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private IEnumerable<EdNode> Ordered()
    {
        EdNode? start = Find(Start);
        if (start != null) { yield return start; }
        foreach (EdNode n in Nodes) { if (n != start) { yield return n; } }
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? (v.GetString() ?? "") : "";
}
