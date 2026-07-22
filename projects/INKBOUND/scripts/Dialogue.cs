using AetherCore;

namespace AetherGame;

/// <summary>Entry point for starting conversations from anywhere (triggers, scripts). Loads the JSON
/// asset, parses it, and hands it to the single active DialogueRunner. The VFS prefix (project:// maps
/// to the project root, assets live under it) was pinned in the dialogue system's Task 1 probe.</summary>
public static class Dialogue
{
    private const string DlgRoot = "project://assets/dialogue/";

    private static DialogueRunner? s_runner;

    public static bool IsActive => s_runner != null && s_runner.Active;

    /// <summary>The node the runner is currently showing (null when idle). Set by DialogueRunner; read
    /// by the graph editor to highlight the live node during play.</summary>
    public static string? CurrentNodeId;

    internal static void Register(DialogueRunner r) => s_runner = r;
    internal static void Unregister(DialogueRunner r) { if (s_runner == r) s_runner = null; }

    public static void Play(string id)
    {
        if (s_runner == null) { Log.Warn("[INKBOUND] Dialogue.Play: no DialogueRunner in scene"); return; }
        if (s_runner.Active) { return; } // one conversation at a time
        string path = $"{DlgRoot}{id}.json";
        string? json = Assets.ReadText(path);
        if (json == null) { Log.Warn($"[INKBOUND] Dialogue.Play: '{path}' not found"); return; }
        DialogueGraph? g = DialogueGraph.Parse(json);
        if (g == null) { return; }
        s_runner.Begin(g);
    }

    public static void SetFlag(string flag) => DialogueState.SetFlag(flag);
    public static bool HasFlag(string flag) => DialogueState.HasFlag(flag);
}
