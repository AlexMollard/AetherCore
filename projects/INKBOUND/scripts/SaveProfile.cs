using System;
using System.Collections.Generic;

namespace AetherGame;

/// <summary>Per-slot persisted progression. Plain data plus pure derivation helpers - no engine
/// calls, so it reasons and serialises cleanly with System.Text.Json. Unlock is DERIVED from
/// completion (never stored): Level1 is always open; every later level opens once the previous is
/// completed.</summary>
public sealed class SaveProfile
{
    /// <summary>Ship order of levels; index order defines the linear unlock chain.</summary>
    public static readonly string[] LevelKeys = { "Level1", "Level2", "Level3", "Level4" };

    public bool Exists { get; set; }
    public string FurthestLevel { get; set; } = "Level1";
    public Dictionary<string, LevelRecord> Levels { get; set; } = new();

    private static readonly LevelRecord s_empty = new();

    public LevelRecord Level(string key)
    {
        if (!Levels.TryGetValue(key, out LevelRecord? r)) { r = new LevelRecord(); Levels[key] = r; }
        return r;
    }

    /// <summary>Read-only lookup that never inserts into <see cref="Levels"/>. Use for pure
    /// queries (IsUnlocked, CompletedCount) so per-frame calls don't fill the save with empty
    /// entries for levels never played. Never mutate the returned record.</summary>
    private LevelRecord Peek(string key) => Levels.TryGetValue(key, out LevelRecord? r) ? r : s_empty;

    public bool IsUnlocked(string key)
    {
        int i = Array.IndexOf(LevelKeys, key);
        if (i < 0) return false;      // unknown key (e.g. Sandbox) never gates through here
        if (i == 0) return true;      // Level1 always open
        return Peek(LevelKeys[i - 1]).Completed;
    }

    public void RecordCompletion(string key, int coins)
    {
        LevelRecord r = Level(key);
        r.Completed = true;
        if (coins > r.BestCoins) r.BestCoins = coins;
        r.FurthestCheckpoint = 0;     // finished: no partial-progress checkpoint to resume to
        Exists = true;
    }

    public void RecordCheckpoint(string key, int index)
    {
        LevelRecord r = Level(key);
        if (index > r.FurthestCheckpoint) r.FurthestCheckpoint = index;
        int ki = Array.IndexOf(LevelKeys, key);
        if (ki >= 0 && ki >= Array.IndexOf(LevelKeys, FurthestLevel)) FurthestLevel = key;
        Exists = true;
    }

    public void RecordBestTime(string key, float seconds)
    {
        LevelRecord r = Level(key);
        if (r.BestTimeSeconds == null || seconds < r.BestTimeSeconds.Value) r.BestTimeSeconds = seconds;
    }

    public int CompletedCount()
    {
        int n = 0;
        foreach (string k in LevelKeys) if (Peek(k).Completed) n++;
        return n;
    }
}

public sealed class LevelRecord
{
    public bool Completed { get; set; }
    public int BestCoins { get; set; }
    public float? BestTimeSeconds { get; set; }
    public int FurthestCheckpoint { get; set; }   // 0 = no checkpoint reached (resume at level start)
}
