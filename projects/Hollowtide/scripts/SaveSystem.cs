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

	/// <summary>True when the vigil on screen came out of a half-finished write rather than the
	/// save proper. Read by nothing yet; set so the game CAN say so rather than pretending
	/// nothing happened.</summary>
	public static bool Salvaged { get; private set; }

	/// <summary>Read a vigil from a file, or null if it is missing or unreadable. Never throws:
	/// every caller is already handling a failure when it asks.</summary>
	private static VigilSave? TryRead(string path)
	{
		try
		{
			return File.Exists(path)
				? JsonSerializer.Deserialize<VigilSave>(File.ReadAllText(path))
				: null;
		}
		catch
		{
			return null;
		}
	}

	/// <summary>
	/// Move a save that could not be read out of the way, so the next one cannot overwrite it.
	/// </summary>
	/// <remarks>
	/// Numbered rather than timestamped, and never overwriting an earlier rescue: somebody whose
	/// save breaks twice has two problems, and losing the first copy while rescuing the second
	/// would be the same fault this exists to prevent.
	/// <para>
	/// Every failure here is swallowed. This runs while the game is already recovering from a
	/// broken save, and a keeper who cannot start their game because the rescue of their old one
	/// failed is worse off than one who simply lost it.
	/// </para>
	/// </remarks>
	private static void SetAside(string path, string why)
	{
		try
		{
			if (!File.Exists(path))
			{
				return;
			}
			// The naming lives in SaveNaming, engine-free, so the harness can hold the one
			// decision here that can destroy data: which name the keeper's only surviving copy
			// is moved to.
			string kept = SaveNaming.Kept(path, File.Exists);
			File.Move(path, kept);
			Log.Warn("[Hollowtide] could not read the save (" + why + "). It has been kept at "
				+ kept + " and a fresh vigil started. Nothing has been deleted.");
		}
		catch (Exception rescue)
		{
			Log.Warn("[Hollowtide] could not read the save (" + why
				+ "), and could not move it aside either: " + rescue.Message);
		}
	}

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
				// Valid JSON that is not a vigil. Just as unusable as a truncated file, and
				// just as worth keeping.
				SetAside(path, "it did not contain a vigil");
				return false;
			}
			Salvaged = false;
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
			// Kept, not stepped over. Returning false starts a fresh vigil, and the first
			// autosave of that vigil would have written straight over the file - so a keeper
			// whose save was truncated by a bad shutdown or a full disk lost everything
			// permanently, and the only trace was one line in a log nobody reads. Moving it
			// aside costs nothing and means the data still exists to be repaired.
			// Before giving up: a save is written to a temporary file and then moved over the
			// real one, so a machine that died in the gap between those two steps leaves a
			// COMPLETE and NEWER vigil sitting in the temporary file. Reading it is the
			// difference between losing a session and losing nothing. It is only ever tried
			// when the real save cannot be read, so an older temporary file can only ever
			// replace something already unusable.
			VigilSave? rescued = TryRead(FilePath() + SaveNaming.TempSuffix);
			if (rescued != null)
			{
				VigilData.Apply(rescued);
				Salvaged = true;
				SetAside(FilePath(), e.Message);
				Log.Warn("[Hollowtide] the save could not be read, but an unfinished write of it"
					+ " could. The vigil has been recovered from it.");
				return true;
			}
			SetAside(FilePath(), e.Message);
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
			string temp = path + SaveNaming.TempSuffix;
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
			// And the half-written one beside it. A save is written to a temporary file and then
			// moved over the real one, so a machine that died in the gap leaves a complete vigil
			// in the temporary file - which is exactly what Load reaches for when the real save
			// cannot be read. Left behind by a wipe, it is a copy of the life the keeper just
			// asked twice to be rid of, waiting for their NEXT save to be interrupted. Narrow,
			// and the one place in the game with no undo behind it.
			//
			// The .broken rescues are deliberately left alone: those are copies kept because
			// something went wrong, and a keeper starting again has not asked for the forensic
			// evidence of the last failure to be destroyed too.
			string half = path + SaveNaming.TempSuffix;
			if (File.Exists(half))
			{
				File.Delete(half);
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
