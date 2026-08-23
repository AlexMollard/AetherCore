using System;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>The on-disk shape of a vigil. Flat, versioned, and all public fields, so the
/// serializer needs nothing configured and a future field is a one-line addition.</summary>
public sealed class VigilSave
{
	// Auto-PROPERTIES, not fields, and that distinction is the whole file working or not:
	// System.Text.Json ignores public fields unless every call site opts in with
	// IncludeFields, and a serializer that quietly writes "{}" is indistinguishable from a
	// game that had nothing to save. Properties need no options object, so the two call
	// sites cannot disagree about it.
	public int Version { get; set; } = 1;
	public string KeeperName { get; set; } = "Keeper";
	public double Ichor { get; set; }
	public double RunIchor { get; set; }
	public double LifetimeIchor { get; set; }
	public int[] Owned { get; set; } = Array.Empty<int>();
	public bool[] Offerings { get; set; } = Array.Empty<bool>();
	public bool[] Marks { get; set; } = Array.Empty<bool>();
	public bool[] Overseers { get; set; } = Array.Empty<bool>();
	public int Sigils { get; set; }
	public int Communions { get; set; }
	public double Dread { get; set; }
	public double PlayedSeconds { get; set; }
	public double HighDreadSeconds { get; set; }
	public int WardsRaised { get; set; }
	public int TimesTaken { get; set; }
	public int CommunionSurges { get; set; }
	public int HandGathers { get; set; }
	public double SharedVigilSeconds { get; set; }
	/// <summary>Unix seconds at the last write. The only thing offline progress is measured
	/// from, and deliberately UTC so a machine changing timezone does not hand out eight hours.</summary>
	public long SavedAtUnix { get; set; }

	// Settings live in the same file: there is one vigil, and a second file to keep in step
	// with it would only be a second thing to go missing.
	public bool ShowWhispers { get; set; } = true;
	public float DreadShake { get; set; } = 1.0f;
}

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

	public static bool ShowWhispers = true;
	public static float DreadShake = 1.0f;

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
			Apply(save);

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
			VigilSave save = Capture();
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
	}

	private static VigilSave Capture() => new VigilSave
	{
		KeeperName = Vigil.KeeperName,
		Ichor = Vigil.Ichor,
		RunIchor = Vigil.RunIchor,
		LifetimeIchor = Vigil.LifetimeIchor,
		Owned = (int[])Vigil.Owned.Clone(),
		Offerings = (bool[])Vigil.OfferingsTaken.Clone(),
		Marks = (bool[])Vigil.MarksEarned.Clone(),
		Overseers = (bool[])Vigil.Overseers.Clone(),
		Sigils = Vigil.Sigils,
		Communions = Vigil.Communions,
		Dread = Vigil.Dread,
		PlayedSeconds = Vigil.PlayedSeconds,
		HighDreadSeconds = Vigil.HighDreadSeconds,
		WardsRaised = Vigil.WardsRaised,
		TimesTaken = Vigil.TimesTaken,
		CommunionSurges = Vigil.CommunionSurges,
		HandGathers = Vigil.HandGathers,
		SharedVigilSeconds = Vigil.SharedVigilSeconds,
		SavedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
		ShowWhispers = ShowWhispers,
		DreadShake = DreadShake,
	};

	private static void Apply(VigilSave save)
	{
		Vigil.KeeperName = string.IsNullOrWhiteSpace(save.KeeperName) ? "Keeper" : save.KeeperName;
		Vigil.Ichor = save.Ichor;
		Vigil.RunIchor = save.RunIchor;
		Vigil.LifetimeIchor = save.LifetimeIchor;
		Vigil.Sigils = save.Sigils;
		Vigil.Communions = save.Communions;
		Vigil.Dread = Math.Clamp(save.Dread, 0.0, 1.0);
		Vigil.PlayedSeconds = save.PlayedSeconds;
		Vigil.HighDreadSeconds = save.HighDreadSeconds;
		Vigil.WardsRaised = save.WardsRaised;
		Vigil.TimesTaken = save.TimesTaken;
		Vigil.CommunionSurges = save.CommunionSurges;
		Vigil.HandGathers = save.HandGathers;
		Vigil.SharedVigilSeconds = save.SharedVigilSeconds;
		ShowWhispers = save.ShowWhispers;
		DreadShake = save.DreadShake;

		// Copied element-wise against the CURRENT table sizes. A save written before a rite or
		// an offering was added is then still a valid save, which is the difference between
		// adding content and invalidating everybody.
		CopyInto(save.Owned, Vigil.Owned);
		CopyInto(save.Offerings, Vigil.OfferingsTaken);
		CopyInto(save.Marks, Vigil.MarksEarned);
		CopyInto(save.Overseers, Vigil.Overseers);
		Vigil.Revision++;
	}

	private static void CopyInto(int[] from, int[] to)
	{
		int n = Math.Min(from.Length, to.Length);
		for (int i = 0; i < n; i++)
		{
			to[i] = from[i];
		}
	}

	private static void CopyInto(bool[] from, bool[] to)
	{
		int n = Math.Min(from.Length, to.Length);
		for (int i = 0; i < n; i++)
		{
			to[i] = from[i];
		}
	}
}
