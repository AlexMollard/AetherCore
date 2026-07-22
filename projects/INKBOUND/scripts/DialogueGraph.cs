using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

public sealed class DialogueChoice
{
    public string Text = "";
    public string Goto = "";
    public string? If;
    public string? Set;
}

public sealed class DialogueNode
{
    public string Speaker = "";
    public string? Portrait;
    public string Text = "";
    public InkEffect Effect = InkEffect.Normal;
    public List<TextRun> Runs = new();
    public string? Goto;
    public List<DialogueChoice> Choices = new();
    public bool HasChoices => Choices.Count > 0;
}

/// <summary>A parsed conversation. Pure data — no engine calls. Built by <see cref="Parse"/> from the
/// JSON schema in the design spec (§2). Returns null on malformed JSON (logged, never throws to caller).</summary>
public sealed class DialogueGraph
{
    public string Id = "";
    public string Start = "";
    public Dictionary<string, DialogueNode> Nodes = new();

    public DialogueNode? NodeOrNull(string? id) =>
        id != null && Nodes.TryGetValue(id, out var n) ? n : null;

    public static DialogueGraph? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            var g = new DialogueGraph
            {
                Id = GetStr(root, "id") ?? "",
                Start = GetStr(root, "start") ?? "",
            };
            if (!root.TryGetProperty("nodes", out JsonElement nodes) || nodes.ValueKind != JsonValueKind.Object)
            {
                Log.Warn("[INKBOUND] dialogue: missing 'nodes' object");
                return null;
            }
            foreach (JsonProperty np in nodes.EnumerateObject())
            {
                JsonElement ne = np.Value;
                var node = new DialogueNode
                {
                    Speaker = GetStr(ne, "speaker") ?? "",
                    Portrait = GetStr(ne, "portrait"),
                    Text = GetStr(ne, "text") ?? "",
                    Goto = GetStr(ne, "goto"),
                };
                if (DialogueMarkup.TryEffect(GetStr(ne, "effect") ?? "normal", out InkEffect eff)) { node.Effect = eff; }
                (_, node.Runs) = DialogueMarkup.Tokenize(node.Text, node.Effect);
                node.Text = StripTags(node.Runs); // plain, tag-free
                if (ne.TryGetProperty("choices", out JsonElement ch) && ch.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement ce in ch.EnumerateArray())
                    {
                        node.Choices.Add(new DialogueChoice
                        {
                            Text = GetStr(ce, "text") ?? "",
                            Goto = GetStr(ce, "goto") ?? "",
                            If = GetStr(ce, "if"),
                            Set = GetStr(ce, "set"),
                        });
                    }
                }
                g.Nodes[np.Name] = node;
            }
            if (g.Start.Length == 0 || !g.Nodes.ContainsKey(g.Start))
            {
                Log.Warn($"[INKBOUND] dialogue '{g.Id}': start node '{g.Start}' missing");
                return null;
            }
            return g;
        }
        catch (JsonException e)
        {
            Log.Warn($"[INKBOUND] dialogue parse failed: {e.Message}");
            return null;
        }
    }

    private static string StripTags(List<TextRun> runs)
    {
        var sb = new StringBuilder();
        foreach (TextRun r in runs) { sb.Append(r.Text); }
        return sb.ToString();
    }

    private static string? GetStr(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
}
