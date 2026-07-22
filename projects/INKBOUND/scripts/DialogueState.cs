using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

/// <summary>Global dialogue variables for a run. Flags persist across conversations (static, like
/// GameState) and are cleared on a fresh run. Conditions are the minimal grammar from the spec:
/// "flag" (set), "!flag" (unset), "coins>=N" (run coin count). Unknown -> false + warning.</summary>
public static class DialogueState
{
    private static readonly HashSet<string> s_flags = new();

    public static void SetFlag(string flag) { if (!string.IsNullOrEmpty(flag)) s_flags.Add(flag); }
    public static bool HasFlag(string flag) => s_flags.Contains(flag);
    public static void ResetRun() => s_flags.Clear();

    /// <summary>Evaluate a condition string. Null/empty => true (no gate).</summary>
    public static bool Evaluate(string? cond)
    {
        if (string.IsNullOrWhiteSpace(cond)) { return true; }
        cond = cond.Trim();

        if (cond.StartsWith("!")) { return !HasFlag(cond.Substring(1).Trim()); }

        int op = cond.IndexOf(">=");
        if (op > 0)
        {
            string lhs = cond.Substring(0, op).Trim();
            if (int.TryParse(cond.Substring(op + 2).Trim(), out int n))
            {
                if (lhs == "coins") { return GameState.TotalCoins >= n; }
            }
            Log.Warn($"[INKBOUND] dialogue: unknown condition '{cond}'");
            return false;
        }

        // Bare token => flag presence.
        if (IsFlagToken(cond)) { return HasFlag(cond); }
        Log.Warn($"[INKBOUND] dialogue: unknown condition '{cond}'");
        return false;
    }

    private static bool IsFlagToken(string s)
    {
        foreach (char c in s) { if (!(char.IsLetterOrDigit(c) || c == '_')) return false; }
        return s.Length > 0;
    }
}
