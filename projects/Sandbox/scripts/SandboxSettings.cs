using System;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Player-facing Sandbox settings: look sensitivity, invert-Y, field of view, and
/// whichever key binding actually flows through <see cref="InputActions"/> today.
/// Persisted to LocalAppData, mirroring INKBOUND's <c>GameSettings.cs</c> and Whisper's
/// <c>WhisperPrefs.cs</c> - the established pattern in this repo for a per-project player
/// preference.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why not <c>SettingsService</c>.</b> Same reasoning as <c>WhisperPrefs.cs</c>'s own
/// file comment, verified directly against <c>SettingsService.hpp</c>/<c>EngineSettings.hpp</c>
/// rather than assumed: that is a fixed-schema ENGINE settings struct (window size, vsync,
/// network defaults) with no managed (C#) binding exported at all - there is no
/// <c>Native.aether_settings_*</c> entry point for a script to call even if this belonged
/// there conceptually, which it does not: adding "Sandbox's look sensitivity" to an engine
/// struct is exactly the game-code-in-the-engine line this project does not cross.
/// </para>
/// <para>
/// Static and loaded lazily for the same reason <c>WhisperPrefs</c> is: a scene load
/// discards whatever screen collected the values (Main Menu's Settings screen and the
/// in-arena Pause Menu's Settings screen are two different scenes' worth of UI), so this
/// has to survive <see cref="Scene.Load"/> on its own rather than living on an entity.
/// </para>
/// </remarks>
public static class SandboxSettings
{
    public const float MinSensitivity = 0.02f;
    public const float MaxSensitivity = 0.60f;
    public const float SensitivityStep = 0.01f;

    public const float MinFov = 60.0f;
    public const float MaxFov = 100.0f;
    public const float FovStep = 2.0f;

    /// <summary>Matches FirstPersonPlayer.LookSensitivity's own compiled-in default, so a
    /// player who never opens Settings sees the exact behaviour they already had.</summary>
    public static float LookSensitivity = 0.15f;

    public static bool InvertY;

    /// <summary>Matches the player prefab's authored camera FOV (player.prefab.toml).</summary>
    public static float FovDegrees = 60.0f;

    /// <summary>Key bound to PropSpawner's "spawn_prop" InputAction - the only binding
    /// actually rebindable today. See UiSettingsScreen's own file comment for why the rest
    /// (WASD, jump, sprint, Q, E) are not offered: they are raw Input.IsKeyDown calls, not
    /// InputActions entries, so rebinding them would need to change nothing here and
    /// everything in those scripts instead.</summary>
    public static Key SpawnPropKey = Key.F;

    private static bool s_loaded;

    /// <summary>Read the file, at most once per process. Every public getter above is a
    /// plain field rather than a property specifically so nothing can forget to call this
    /// first - callers are expected to call it once, early (Main Menu's Settings screen and
    /// the HUD's per-frame apply both do), matching WhisperPrefs' own convention.</summary>
    public static void EnsureLoaded()
    {
        if (s_loaded)
        {
            return;
        }
        s_loaded = true; // set FIRST: a failed read must not be retried every frame
        try
        {
            string path = PathOnDisk();
            if (!File.Exists(path))
            {
                return;
            }
            Dto? dto = JsonSerializer.Deserialize<Dto>(File.ReadAllText(path));
            if (dto == null)
            {
                return;
            }
            LookSensitivity = Math.Clamp(dto.Sensitivity, MinSensitivity, MaxSensitivity);
            InvertY = dto.InvertY;
            FovDegrees = Math.Clamp(dto.Fov, MinFov, MaxFov);
            if (Enum.IsDefined(typeof(Key), dto.SpawnPropKey))
            {
                SpawnPropKey = (Key)dto.SpawnPropKey;
            }
        }
        catch (Exception e)
        {
            // A corrupt or unreadable settings file is not worth refusing to start over -
            // the defaults above are already in place, so there is nothing to roll back.
            Log.Warn($"[Sandbox] settings load failed: {e.Message}");
        }
    }

    /// <summary>Write the current values out. Cheap enough to call whenever a field is
    /// committed - a setting the player has to press Save to keep is a setting they will
    /// lose.</summary>
    public static void Save()
    {
        try
        {
            Dto dto = new()
            {
                Sensitivity = LookSensitivity,
                InvertY = InvertY,
                Fov = FovDegrees,
                SpawnPropKey = (int)SpawnPropKey,
            };
            File.WriteAllText(PathOnDisk(), JsonSerializer.Serialize(dto));
        }
        catch (Exception e)
        {
            Log.Warn($"[Sandbox] settings save failed: {e.Message}");
        }
    }

    private static string PathOnDisk()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "Sandbox");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "settings.json");
    }

    /// <summary>On-disk shape. Separate from the fields above so renaming one does not
    /// silently orphan everybody's saved settings.</summary>
    private sealed class Dto
    {
        public float Sensitivity { get; set; }
        public bool InvertY { get; set; }
        public float Fov { get; set; }
        public int SpawnPropKey { get; set; }
    }
}
