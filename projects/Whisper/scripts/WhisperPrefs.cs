using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The two answers the connect screen should not have to ask twice: who this player is,
/// and which servers they have been on lately. Written to LocalAppData, read once on the
/// first menu entry of a process.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why not the engine's settings system.</b> <c>SettingsService</c> is a fixed-schema
/// struct of ENGINE settings - window size, vsync, the startup scene - cascaded through a
/// shipped <c>engine.toml</c> into a user <c>settings.toml</c>. Adding "the Whisper
/// player's name" to it means adding a Whisper field to an engine struct, which is exactly
/// the game-code-in-the-engine line the project does not cross. It is the right store for
/// things the ENGINE reads and the wrong one for things a GAME does.
/// </para>
/// <para>
/// <b>Why not a new format.</b> This is not one: it is INKBOUND's
/// <c>GameSettings</c>/<c>SaveSystem</c> pattern - BCL <c>System.Text.Json</c> into
/// <c>%LocalAppData%/AetherCore/&lt;project&gt;/</c> - applied to a second project, the
/// established prior art for a per-project player preference in this repo.
/// </para>
/// <para>
/// Static, and loaded lazily, for the same reason <see cref="NetSession"/> is: a scene load
/// discards the screen that collected the answers. Statics survive the editor's Play/Stop
/// cycle within one process, so only a genuine relaunch re-reads the file - which is what
/// makes a restart the honest test of whether the WRITE happened.
/// </para>
/// </remarks>
public static class WhisperPrefs
{
    /// <summary>How many servers are offered for re-selection. Four fits one screen
    /// without becoming a list that needs managing.</summary>
    public const int RecentCapacity = 4;

    /// <summary>Longest player name kept, in characters. The length cap half of
    /// <see cref="Clean"/>; exposed so the connect screen can tell its text box the
    /// same limit it is about to enforce.</summary>
    public const int MaxNameLength = 64;

    private static readonly List<string> s_recent = new();
    private static bool s_loaded;

    /// <summary>The name last committed on the connect screen. Empty until the player has
    /// chosen one, so a first run shows the field's placeholder rather than "Player".</summary>
    public static string PlayerName { get; set; } = string.Empty;

    /// <summary>Servers joined most recently first, newest at index 0.</summary>
    public static IReadOnlyList<string> Recent
    {
        get
        {
            EnsureLoaded();
            return s_recent;
        }
    }

    /// <summary>Read the file, at most once per process.</summary>
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
            PlayerName = Clean(dto.Name);
            s_recent.Clear();
            if (dto.Recent != null)
            {
                foreach (string entry in dto.Recent)
                {
                    Remember(entry);
                }
            }
        }
        catch (Exception e)
        {
            // A corrupt or unreadable prefs file is not worth refusing to start over. The
            // defaults above are already in place, so there is nothing to roll back.
            Log.Warn($"[Whisper] prefs load failed: {e.Message}");
        }
    }

    /// <summary>Move <paramref name="address"/> to the front of the recent list, dropping
    /// the oldest once the list is full.</summary>
    /// <remarks>
    /// Most-recently-used rather than append-only: a player who alternates between two
    /// servers should see both at the top, not watch the one they use most sink. Matching
    /// is case-insensitive because a host name is, and a blank is ignored rather than
    /// recorded - "no address" means the loopback default and is not somewhere the player
    /// chose to go.
    /// </remarks>
    public static void Remember(string address)
    {
        EnsureLoaded();
        string cleaned = Clean(address);
        if (cleaned.Length == 0)
        {
            return;
        }
        s_recent.RemoveAll(e => string.Equals(e, cleaned, StringComparison.OrdinalIgnoreCase));
        s_recent.Insert(0, cleaned);
        while (s_recent.Count > RecentCapacity)
        {
            s_recent.RemoveAt(s_recent.Count - 1);
        }
    }

    /// <summary>Write the current values out. Cheap enough to call whenever a field is
    /// committed; a preference the player has to press Save to keep is a preference they
    /// will lose.</summary>
    public static void Save()
    {
        try
        {
            Dto dto = new() { Name = PlayerName, Recent = s_recent.ToArray() };
            File.WriteAllText(PathOnDisk(), JsonSerializer.Serialize(dto));
        }
        catch (Exception e)
        {
            Log.Warn($"[Whisper] prefs save failed: {e.Message}");
        }
    }

    /// <summary>Where the file lives, creating the directory on the way.</summary>
    public static string PathOnDisk()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "Whisper");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "prefs.json");
    }

    /// <summary>Trim, and refuse anything the ASCII-only font pipeline cannot draw:
    /// the same contract for a name going live this session as for one coming back
    /// off disk.</summary>
    /// <remarks>
    /// Public because the connect screen's live commit is the other side of the same
    /// rule: <see cref="PlayerName"/> cleaned only on load would let a name typed
    /// this session reach every peer's transcript with glyphs the font cannot draw,
    /// exactly as though it had come off a hand-edited file.
    /// </remarks>
    public static string Clean(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }
        Span<char> buffer = stackalloc char[MaxNameLength];
        int n = 0;
        foreach (char c in value.Trim())
        {
            if (n == buffer.Length)
            {
                break;
            }
            if (c >= ' ' && c <= '~')
            {
                buffer[n++] = c;
            }
        }
        return new string(buffer[..n]);
    }

    /// <summary>On-disk shape. Separate from the properties above so renaming one does not
    /// silently orphan everybody's saved preferences.</summary>
    private sealed class Dto
    {
        public string? Name { get; set; }
        public string[]? Recent { get; set; }
    }
}
