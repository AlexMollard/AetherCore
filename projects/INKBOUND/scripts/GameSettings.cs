using System;
using System.IO;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>Player-facing game settings, persisted to LocalAppData. Read by the
/// Settings screen and by gameplay (Ink Glow, Screen Shake).</summary>
public static class GameSettings
{
    public static float MusicVolume = 0.70f;
    public static float SfxVolume = 0.55f;
    public static float InkGlow = 0.85f;      // 0..1, scales the ink accent glow
    public static bool ScreenShake = true;

    /// <summary>The cyan ink accent used across the UI and ink mechanic.</summary>
    public static readonly Vector4 Accent = new(0.302f, 0.851f, 1.0f, 1.0f);

    private static string PathOnDisk()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "INKBOUND");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "settings.json");
    }

    public static float ClampUnit(float v) => v < 0f ? 0f : (v > 1f ? 1f : v);

    private sealed class Dto
    {
        public float Music { get; set; }
        public float Sfx { get; set; }
        public float InkGlow { get; set; }
        public bool ScreenShake { get; set; }
    }

    public static void Load()
    {
        try
        {
            string p = PathOnDisk();
            if (!File.Exists(p)) return;
            Dto? d = JsonSerializer.Deserialize<Dto>(File.ReadAllText(p));
            if (d == null) return;
            MusicVolume = ClampUnit(d.Music);
            SfxVolume = ClampUnit(d.Sfx);
            InkGlow = ClampUnit(d.InkGlow);
            ScreenShake = d.ScreenShake;
        }
        catch (Exception e) { Log.Warn($"[INKBOUND] settings load failed: {e.Message}"); }
    }

    public static void Save()
    {
        try
        {
            var d = new Dto { Music = MusicVolume, Sfx = SfxVolume, InkGlow = InkGlow, ScreenShake = ScreenShake };
            File.WriteAllText(PathOnDisk(), JsonSerializer.Serialize(d));
        }
        catch (Exception e) { Log.Warn($"[INKBOUND] settings save failed: {e.Message}"); }
    }
}
