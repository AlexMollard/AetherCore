using AetherCore;

namespace AetherGame;

// TEMPORARY in-editor test harness (no C# unit runner exists). Attach to any entity, Play,
// read the console for [DLGTEST] PASS/FAIL lines, then remove after Task 3.
public sealed class DialogueSelfTest : EntityScript
{
    private int _pass, _fail;
    private void Check(bool cond, string label)
    {
        if (cond) { _pass++; Log.Info($"[DLGTEST] PASS {label}"); }
        else      { _fail++; Log.Warn($"[DLGTEST] FAIL {label}"); }
    }

    public override void OnAttach()
    {
        const string json = @"{ ""id"":""t"", ""start"":""a"", ""nodes"": {
            ""a"": { ""speaker"":""V"", ""text"":""go [shake]down[/shake] now"", ""effect"":""whisper"",
                     ""choices"":[ {""text"":""x"",""goto"":""b"",""set"":""f""},
                                   {""text"":""y"",""goto"":""c"",""if"":""coins>=3""} ] },
            ""b"": { ""speaker"":""V"", ""text"":""deeper"", ""goto"":""end"" } } }";

        DialogueGraph? g = DialogueGraph.Parse(json);
        Check(g != null, "parse ok");
        Check(g!.Nodes.Count == 2, "node count 2");
        DialogueNode a = g.Nodes["a"];
        Check(a.Text == "go down now", "tags stripped from plain text");
        Check(a.Runs.Count == 3, "three runs (whisper|shake|whisper)");
        Check(a.Runs[1].Effect == InkEffect.Shake, "middle run is shake");
        Check(a.Runs[0].Effect == InkEffect.Whisper, "outer runs take node default whisper");
        Check(a.Choices.Count == 2, "two choices");
        Check(a.Choices[1].If == "coins>=3", "choice condition parsed");
        Check(DialogueGraph.Parse("{ not json") == null, "malformed -> null");

        // DialogueState checks are added in Task 3.
        Log.Info($"[DLGTEST] DONE pass={_pass} fail={_fail}");
    }
}
