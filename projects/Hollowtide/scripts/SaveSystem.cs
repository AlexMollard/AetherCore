using System;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Reads and writes the single save under LocalAppData, and turns the gap since the last
/// write into offline progress.
/// </summary>
/// <remarks>
/// Under LocalAppData rather than in the project, so a published build and the editor share
/// one vigil and neither writes into a folder the player may not own. A corrupt or partial
/// file is reported and then ignored - a keeper who loses a save to a half-written JSON
/// blob should get a fresh parish, not a crash on the loading screen.
/// </remarks>
public static class SaveSystem
{
	private const string kFileName = "vigil.json";

	/// <summary>Filled in by <see cref="Load"/> when time had passed. The game layer shows it
	/// once and clears it.</summary>
	public static OfflineReport? PendingOffline;

	/// <summary>Forwarded to <see cref="VigilData"/>, which is where the save's contents are
	/// decided. Kept under this name because every screen already asks SaveSystem for them.</summary>
	public static bool ShowWhispers
	{
		get => VigilData.ShowWhispers;
		set => VigilData.ShowWhispers = value;
	}

	public static float DreadShake
	{
		get => VigilData.DreadShake;
		set => VigilData.DreadShake = value;
	}

	/// <summary>True once <see cref="EnsureLoaded"/> has run in this process.</summary>
	public static bool Loaded { get; private set; }

	/// <summary>True when the load found a save to read.</summary>
	public static bool HadSave { get; private set; }

	/// <summary>
	/// Load exactly once per process, whichever screen asks first.
	/// </summary>
	/// <remarks>
	/// Both the threshold and the vigil need the save - one to fill the name field, the
	/// other to run the parish - and both are entered on a cold start. Loading twice would
	/// replay the offline catch-up against the same timestamp and report it twice, so the
	/// guard lives HERE rather than in either caller: a third screen that needs the save
	/// cannot reintroduce the bug by forgetting to check a flag it does not own.
	/// </remarks>
	public static bool EnsureLoaded()
	{
		if (!Loaded)
		{
			Loaded = true;
			HadSave = Load();
		}
		return HadSave;
	}

	private static string Dir()
	{
		string dir = Path.Combine(
			Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
			"AetherCore", "Hollowtide");
		Directory.CreateDirectory(dir);
		return dir;
	}

	private static string FilePath() => Path.Combine(Dir(), kFileName);

	public static bool Exists()
	{
		try
		{
			return File.Exists(FilePath());
		}
		catch (Exception e)
		{
			Log.Warn("[Hollowtide] save probe failed: " + e.Message);
			return false;
		}
	}

	/// <summary>Load the vigil, then replay the time the game was closed for. Returns false
	/// when there was nothing to load, which is how the caller knows to start fresh.</summary>
	public static bool Load()
	{
		try
		{
			string path = FilePath();
			if (!File.Exists(path))
			{
				return false;
			}
			VigilSave? save = JsonSerializer.Deserialize<VigilSave>(File.ReadAllText(path));
			if (save == null)
			{
				return false;
			}
			VigilData.Apply(save);

			long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
			double away = now - save.SavedAtUnix;
			// A clock that went backwards (timezone edit, a restored machine) reads as a
			// negative gap. Treat it as zero rather than as free progress or a negative purse.
			if (away > 30.0)
			{
				OfflineReport report = Vigil.CatchUp(away);
				if (report.Ichor > 0.0)
				{
					PendingOffline = report;
				}
			}
			return true;
		}
		catch (Exception e)
		{
			Log.Warn("[Hollowtide] save load failed, starting a fresh vigil: " + e.Message);
			return false;
		}
	}

	public static void Save()
	{
		try
		{
			VigilSave save = VigilData.Capture();
			// Write beside the real file and move into place: a crash mid-write then costs
			// the newest save rather than every save.
			string path = FilePath();
			string temp = path + ".tmp";
			File.WriteAllText(temp, JsonSerializer.Serialize(save, new JsonSerializerOptions { WriteIndented = false }));
			File.Move(temp, path, overwrite: true);
		}
		catch (Exception e)
		{
			Log.Warn("[Hollowtide] save failed: " + e.Message);
		}
	}

	/// <summary>Forget that a load happened, so the next <see cref="EnsureLoaded"/> reads the
	/// file again. Only a wipe needs this.</summary>
	public static void Forget() => Loaded = false;

	public static void Wipe()
	{
		try
		{
			string path = FilePath();
			if (File.Exists(path))
			{
				File.Delete(path);
			}
		}
		catch (Exception e)
		{
			Log.Warn("[Hollowtide] wipe failed: " + e.Message);
		}
		Vigil.Reset();
		// Forget the load as well as the file. Without this the process still believes it has
		// read a save, so HadSave stays true and the threshold goes on reporting the standing
		// of a keeper who no longer exists.
		Forget();
		EnsureLoaded();
	}

}
