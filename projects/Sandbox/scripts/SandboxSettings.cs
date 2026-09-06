using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Player-facing Sandbox settings: look sensitivity, invert-Y, field of view, and every
/// key binding that flows through <see cref="InputActions"/>. Persisted to LocalAppData,
/// mirroring INKBOUND's <c>GameSettings.cs</c> and Whisper's <c>WhisperPrefs.cs</c> - the
/// established pattern in this repo for a per-project player preference.
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
/// <para>
/// <b>One binding table, not one field per action.</b> <see cref="Bindings"/> is the single
/// source of truth for every rebindable action's <see cref="InputActions"/> name, its
/// settings-screen label, and its compiled-in default key; <see cref="BoundKeys"/> holds
/// the live value per action. Adding a new rebindable action is one row in
/// <see cref="Bindings"/> plus that action's own script calling
/// <c>InputActions.Register(action, SandboxSettings.BoundKeys[action])</c> on attach (see
/// <see cref="PropSpawner"/>'s own file comment for why that call belongs in EVERY attach,
/// not just the first) - never a change to this file's load/save machinery, which is
/// generic over whatever <see cref="Bindings"/> lists. WASD movement is deliberately not
/// in this table: it is a continuous two-axis read (<c>Input.GetAxisRaw</c>), not a
/// boolean action <see cref="InputActions"/> models, and letting a player rebind movement
/// away from WASD is not something this sandbox's design asked for - see the settings
/// screen's own file comment for the full reasoning.
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

    /// <summary>Every rebindable action, in the order the Settings screen lists them:
    /// the <see cref="InputActions"/> name a script registers/reads, the label the
    /// Controls section shows next to it, and the compiled-in default key. "Interact"/
    /// "Tool Fire" match ToolGun's own former <c>InteractKey</c>/<c>ToolFireKey</c> public
    /// fields (now routed through here instead, so <see cref="UiHud"/> and this screen
    /// agree on the current key from one place); "Jump"/"Sprint" match FirstPersonPlayer's
    /// former raw <c>Key.Space</c>/<c>Key.LeftShift</c> checks; "Spawn Menu" matches
    /// SpawnMenu's former raw <c>Key.Q</c> check; "Rotate Held Prop" matches PhysicsGun's
    /// former raw <c>Key.E</c> check.</summary>
    public static readonly (string Action, string Label, Key Default)[] Bindings =
    {
        ("spawn_prop", "Spawn Prop", Key.F),
        ("open_menu", "Spawn Menu", Key.Q),
        ("interact", "Interact", Key.G),
        ("tool_fire", "Tool Fire", Key.T),
        ("rotate_prop", "Rotate Held Prop", Key.E),
        ("jump", "Jump", Key.Space),
        ("sprint", "Sprint", Key.LeftShift),
    };

    /// <summary>Current key per action, keyed by <see cref="Bindings"/>' action name.
    /// Seeded with every default in <see cref="EnsureLoaded"/> before the on-disk file (if
    /// any) overrides individual entries - a binding added to <see cref="Bindings"/> after
    /// a player's last save still gets its compiled-in default instead of silently missing
    /// from the dictionary.</summary>
    public static readonly Dictionary<string, Key> BoundKeys = new();

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
        foreach ((string action, _, Key def) in Bindings)
        {
            BoundKeys[action] = def;
        }
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
            if (dto.Bindings != null)
            {
                foreach ((string action, int keyValue) in dto.Bindings)
                {
                    // Only apply to an action this build still knows about, and only a
                    // value that is still a real Key - either guards against a settings
                    // file written by a future/older build carrying an action this build
                    // removed, or a key value this build's Key enum no longer defines.
                    if (BoundKeys.ContainsKey(action) && Enum.IsDefined(typeof(Key), keyValue))
                    {
                        BoundKeys[action] = (Key)keyValue;
                    }
                }
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
            Dictionary<string, int> bindings = new();
            foreach ((string action, _, _) in Bindings)
            {
                bindings[action] = (int)BoundKeys[action];
            }
            Dto dto = new()
            {
                Sensitivity = LookSensitivity,
                InvertY = InvertY,
                Fov = FovDegrees,
                Bindings = bindings,
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
        public Dictionary<string, int>? Bindings { get; set; }
    }
}
