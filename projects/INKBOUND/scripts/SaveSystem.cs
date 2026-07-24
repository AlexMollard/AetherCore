using System;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>Owns on-disk save state: three slot files under LocalAppData, mirroring GameSettings'
/// pattern. Loads once on first menu entry; the active slot receives every autosave.</summary>
public static class SaveSystem
{
    public const int SlotCount = 3;

    private static readonly SaveProfile[] s_slots = new SaveProfile[SlotCount];
    private static bool s_loaded;

    public static int ActiveIndex { get; private set; } = -1;
    public static SaveProfile? Active => ActiveIndex >= 0 ? s_slots[ActiveIndex] : null;
    public static SaveProfile Slot(int i) => s_slots[i];

    private static string Dir()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "INKBOUND");
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static string PathFor(int i) => Path.Combine(Dir(), $"slot{i}.json");

    public static void EnsureLoaded() { if (!s_loaded) { LoadAll(); s_loaded = true; } }

    public static void LoadAll() { for (int i = 0; i < SlotCount; i++) s_slots[i] = LoadOne(i); }

    private static SaveProfile LoadOne(int i)
    {
        try
        {
            string p = PathFor(i);
            if (!File.Exists(p)) return new SaveProfile { Exists = false };
            SaveProfile? prof = JsonSerializer.Deserialize<SaveProfile>(File.ReadAllText(p));
            return prof ?? new SaveProfile { Exists = false };
        }
        catch (Exception e)
        {
            Log.Warn($"[INKBOUND] slot {i} load failed: {e.Message}");
            return new SaveProfile { Exists = false };
        }
    }

    public static void SetActive(int i) => ActiveIndex = i;

    public static void SaveActive()
    {
        if (ActiveIndex < 0) return;
        try { File.WriteAllText(PathFor(ActiveIndex), JsonSerializer.Serialize(s_slots[ActiveIndex])); }
        catch (Exception e) { Log.Warn($"[INKBOUND] slot {ActiveIndex} save failed: {e.Message}"); }
    }

    public static void Wipe(int i)
    {
        s_slots[i] = new SaveProfile { Exists = false };
        try { File.Delete(PathFor(i)); }
        catch (Exception e) { Log.Warn($"[INKBOUND] slot {i} wipe failed: {e.Message}"); }
    }
}
