using System;

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

	/// <summary>Which rite this run consecrated, or -1. Defaults to -1 rather than 0, so a save
	/// written before consecration existed loads as "not yet chosen" and not as a keeper who
	/// silently consecrated their first rite.</summary>
	public int Consecrated { get; set; } = -1;

	/// <summary>Counters the marks ask about. All default to nothing, so a save written before
	/// they existed loads as a keeper who has not done these things yet - which is true.</summary>
	public int RelicsRendered { get; set; }
	public int BestRelicGrade { get; set; } = -1;
	public int Consecrations { get; set; }

	/// <summary>
	/// What the parish has said, oldest first.
	/// </summary>
	/// <remarks>
	/// Saved because "the messages disappear forever and I cannot read them after they are
	/// gone" is not answered by a transcript that empties when the game closes. Bounded by
	/// Transcript.Capacity, so this cannot grow with playtime - a save that gets bigger the
	/// longer somebody enjoys the game is a punishment for playing it.
	///
	/// Three parallel arrays rather than one array of a small class, matching how everything
	/// else in this file is stored, and tolerant of being ragged: see Transcript.Restore.
	/// </remarks>
	public string[] Spoken { get; set; } = Array.Empty<string>();
	public int[] SpokenOmens { get; set; } = Array.Empty<int>();
	public double[] SpokenAt { get; set; } = Array.Empty<double>();
	public bool[] Marks { get; set; } = Array.Empty<bool>();
	public bool[] Overseers { get; set; } = Array.Empty<bool>();
	/// <summary>Levels held in each boon. Bought with sigils and kept through communion, so
	/// losing them to a reload would undo hours of prestige rather than one run.</summary>
	public int[] Boons { get; set; } = Array.Empty<int>();
	public int Sigils { get; set; }
	/// <summary>Sigils ever taken. Absent from saves written before the balance and the score
	/// were separated, which is why loading floors it at the balance.</summary>
	public int SigilsEarned { get; set; }
	public int Communions { get; set; }
	public double Dread { get; set; }
	/// <summary>Wards in hand. Saved because they are bought, and losing paid-for protection
	/// to a reload would be a charge the player never agreed to.</summary>
	public int Wards { get; set; }
	/// <summary>What is left of a visitation's aftermath. Saved so quitting is not a way to
	/// skip the one cost a visitation has.</summary>
	public double Aftermath { get; set; }
	public double PlayedSeconds { get; set; }
	public double HighDreadSeconds { get; set; }
	public int WardsRaised { get; set; }
	public int TimesTaken { get; set; }
	/// <summary>Visitors turned away by naming what they wanted.</summary>
	public int VisitorsAnswered { get; set; }
	/// <summary>Visitors met, and visitors named. Kept through communion: what the keeper has
	/// learned is not part of the parish they gave back.</summary>
	public bool[] VisitorsMet { get; set; } = Array.Empty<bool>();
	public bool[] VisitorsBested { get; set; } = Array.Empty<bool>();

	/// <summary>The keepers this one used to be, and what each is carrying. Two parallel arrays
	/// rather than an array of Echo, deliberately: Echo has public FIELDS, and this file exists
	/// partly because System.Text.Json silently ignores fields - a nested type would serialise
	/// as a row of empty objects and nobody would find out until a save came back blank.</summary>
	public string[] EchoNames { get; set; } = Array.Empty<string>();
	public double[] EchoBurdens { get; set; } = Array.Empty<double>();

	/// <summary>Relics carried and relics worn, as seeds and grades. A relic is entirely
	/// derived from those two numbers, so this is the whole of it - the name, the powers and
	/// the art are all regenerated rather than stored, which is what keeps an endless supply of
	/// items from becoming an endless save file.</summary>
	public int[] SatchelSeeds { get; set; } = Array.Empty<int>();
	public int[] SatchelGrades { get; set; } = Array.Empty<int>();
	public int[] WornSeeds { get; set; } = Array.Empty<int>();
	public int[] WornGrades { get; set; } = Array.Empty<int>();
	public int RelicsFound { get; set; }
	public int RelicSeed { get; set; }
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
/// Turning a live vigil into a save and back again.
/// </summary>
/// <remarks>
/// Split out of <see cref="SaveSystem"/> so it can be TESTED. Reading and writing the file
/// needs a disk and a real user profile; deciding what goes into it and what comes back out
/// needs neither, and that half is where the danger is - a field captured and not applied, or
/// applied without a clamp, silently costs a keeper everything they have. Nothing in here
/// touches the engine or the filesystem, so the balance harness can round-trip a whole vigil
/// through JSON in memory without going anywhere near the player's actual save.
/// </remarks>
public static class VigilData
{
	/// <summary>
	/// The two settings, kept here rather than with the file IO.
	/// </summary>
	/// <remarks>
	/// They live in the same save as the vigil, so they belong with the code that decides what
	/// a save contains. SaveSystem still exposes them under its own name, because that is where
	/// every other script already reaches for them and a rename would be churn for nothing.
	/// </remarks>
	public static bool ShowWhispers = true;
	public static float DreadShake = 1.0f;

	public static VigilSave Capture() => new VigilSave
	{
		KeeperName = Vigil.KeeperName,
		Ichor = Vigil.Ichor,
		RunIchor = Vigil.RunIchor,
		LifetimeIchor = Vigil.LifetimeIchor,
		Owned = (int[])Vigil.Owned.Clone(),
		Offerings = (bool[])Vigil.OfferingsTaken.Clone(),
		Consecrated = Vigil.Consecrated,
		RelicsRendered = Vigil.RelicsRendered,
		BestRelicGrade = Vigil.BestRelicGrade,
		Consecrations = Vigil.Consecrations,
		Spoken = Transcript.Capture().Lines,
		SpokenOmens = Transcript.Capture().Omens,
		SpokenAt = Transcript.Capture().At,
		Marks = (bool[])Vigil.MarksEarned.Clone(),
		Overseers = (bool[])Vigil.Overseers.Clone(),
		Boons = (int[])Vigil.Boons.Clone(),
		Sigils = Vigil.Sigils,
		SigilsEarned = Vigil.SigilsEarned,
		Communions = Vigil.Communions,
		Dread = Vigil.Dread,
		Wards = Vigil.Wards,
		Aftermath = Vigil.AftermathSeconds,
		PlayedSeconds = Vigil.PlayedSeconds,
		HighDreadSeconds = Vigil.HighDreadSeconds,
		WardsRaised = Vigil.WardsRaised,
		TimesTaken = Vigil.TimesTaken,
		VisitorsAnswered = Vigil.VisitorsAnswered,
		VisitorsMet = (bool[])Vigil.VisitorsMet.Clone(),
		VisitorsBested = (bool[])Vigil.VisitorsBested.Clone(),
		EchoNames = Vigil.Echoes.ConvertAll(e => e.Name).ToArray(),
		EchoBurdens = Vigil.Echoes.ConvertAll(e => e.Burden).ToArray(),
		SatchelSeeds = Vigil.Satchel.ConvertAll(r => r.Seed).ToArray(),
		SatchelGrades = Vigil.Satchel.ConvertAll(r => (int)r.Grade).ToArray(),
		WornSeeds = Array.ConvertAll(Vigil.Worn, r => r.Seed),
		WornGrades = Array.ConvertAll(Vigil.Worn, r => (int)r.Grade),
		RelicsFound = Vigil.RelicsFound,
		RelicSeed = Vigil.RelicSeed,
		CommunionSurges = Vigil.CommunionSurges,
		HandGathers = Vigil.HandGathers,
		SharedVigilSeconds = Vigil.SharedVigilSeconds,
		SavedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
		ShowWhispers = ShowWhispers,
		DreadShake = DreadShake,
	};

	/// <summary>A quantity off the file: never negative, never a NaN or an infinity. JSON can
	/// carry all three and the arithmetic downstream cannot survive any of them.</summary>
	private static double Clean(double value)
		=> double.IsNaN(value) || double.IsInfinity(value) ? 0.0 : Math.Max(0.0, value);

	public static void Apply(VigilSave save)
	{
		Vigil.KeeperName = string.IsNullOrWhiteSpace(save.KeeperName) ? "Keeper" : save.KeeperName;
		Vigil.Ichor = Clean(save.Ichor);
		Vigil.RunIchor = Clean(save.RunIchor);
		Vigil.LifetimeIchor = Clean(save.LifetimeIchor);
		// Clamped, like everything else off the file. A save is a text document on somebody's
		// disk: it can be edited, truncated by a full drive, or written by an older build that
		// did not have a field. Negative sigils are not exploitable - nothing can be bought with
		// them - but they are a state the rules say cannot exist, and the moment one impossible
		// value is tolerated the next one has precedent.
		Vigil.Sigils = Math.Max(0, save.Sigils);
		// An older save has no earned count, only a balance. Reading zero there would wipe the
		// permanent multiplier off a keeper who had already earned it, so the balance is the
		// floor: worst case an old keeper is credited exactly what they still hold.
		Vigil.SigilsEarned = Math.Max(save.SigilsEarned, save.Sigils);
		Vigil.Communions = Math.Max(0, save.Communions);
		Vigil.Dread = Math.Clamp(save.Dread, 0.0, 1.0);
		// Before the ward clamp below, and that order matters: the ward cap is itself a boon,
		// so clamping first would confiscate the wards a Deeper Wards keeper was carrying.
		CopyInto(save.Boons, Vigil.Boons);
		Vigil.Wards = Math.Clamp(save.Wards, 0, Vigil.MaxWards);
		Vigil.AftermathSeconds = Math.Max(0.0, save.Aftermath);
		Vigil.PlayedSeconds = Math.Max(0.0, save.PlayedSeconds);
		Vigil.HighDreadSeconds = Math.Max(0.0, save.HighDreadSeconds);
		Vigil.WardsRaised = Math.Max(0, save.WardsRaised);
		Vigil.TimesTaken = Math.Max(0, save.TimesTaken);
		Vigil.VisitorsAnswered = Math.Max(0, save.VisitorsAnswered);
		Vigil.CommunionSurges = Math.Max(0, save.CommunionSurges);
		Vigil.HandGathers = Math.Max(0, save.HandGathers);
		Vigil.SharedVigilSeconds = Math.Max(0.0, save.SharedVigilSeconds);
		ShowWhispers = save.ShowWhispers;
		DreadShake = save.DreadShake;

		// Copied element-wise against the CURRENT table sizes. A save written before a rite or
		// an offering was added is then still a valid save, which is the difference between
		// adding content and invalidating everybody.
		CopyInto(save.Owned, Vigil.Owned);
		CopyInto(save.Offerings, Vigil.OfferingsTaken);
		// Clamped, because everything read off a file is: a hand-edited or corrupt value here
		// would index the rite tables directly.
		Vigil.Consecrated = save.Consecrated >= 0 && save.Consecrated < Content.RiteCount
			? save.Consecrated : -1;
		Vigil.RelicsRendered = Math.Max(0, save.RelicsRendered);
		Vigil.BestRelicGrade = Math.Clamp(save.BestRelicGrade, -1, (int)Grade.Hollowed);
		Vigil.Consecrations = Math.Max(0, save.Consecrations);
		Transcript.Restore(save.Spoken, save.SpokenOmens, save.SpokenAt);
		CopyInto(save.Marks, Vigil.MarksEarned);
		CopyInto(save.Overseers, Vigil.Overseers);
		// Rebuilt from the shorter of the two, so a half-written save cannot produce an echo
		// with a name and no burden or the reverse.
		Vigil.Echoes.Clear();
		int echoes = Math.Min(save.EchoNames.Length, save.EchoBurdens.Length);
		for (int i = 0; i < echoes && i < Vigil.MaxEchoes; i++)
		{
			Vigil.Echoes.Add(new Echo
			{
				Name = string.IsNullOrWhiteSpace(save.EchoNames[i]) ? "Keeper" : save.EchoNames[i],
				Burden = Math.Clamp(save.EchoBurdens[i], 0.0, Vigil.kEchoCapacity),
			});
		}

		Vigil.RelicsFound = Math.Max(0, save.RelicsFound);
		// Never lower than what is already in hand: the seed counter is what stops two relics
		// ever being the same object, so a save written before it existed must not hand out
		// seeds that are already spoken for.
		Vigil.RelicSeed = Math.Max(save.RelicSeed, HighestSeed(save));

		Vigil.Satchel.Clear();
		int carried = Math.Min(save.SatchelSeeds.Length, save.SatchelGrades.Length);
		for (int i = 0; i < carried && Vigil.Satchel.Count < Relics.Satchel; i++)
		{
			Vigil.Satchel.Add(ReadRelic(save.SatchelSeeds[i], save.SatchelGrades[i]));
		}

		Array.Clear(Vigil.Worn, 0, Vigil.Worn.Length);
		int worn = Math.Min(save.WornSeeds.Length, save.WornGrades.Length);
		for (int i = 0; i < worn && i < Relics.Slots; i++)
		{
			Vigil.Worn[i] = ReadRelic(save.WornSeeds[i], save.WornGrades[i]);
		}

		CopyInto(save.VisitorsMet, Vigil.VisitorsMet);
		CopyInto(save.VisitorsBested, Vigil.VisitorsBested);
		Vigil.Revision++;
	}

	/// <summary>One relic off the wire, with its grade clamped to something that exists. A
	/// grade out of range would index the name tables and the art shader with a number neither
	/// was written for.</summary>
	private static Relic ReadRelic(int seed, int grade) => new Relic
	{
		Seed = seed,
		Grade = (Grade)Math.Clamp(grade, 0, (int)Grade.Hollowed),
	};

	/// <summary>The largest seed anywhere in a save, so the counter can be floored above it.</summary>
	private static int HighestSeed(VigilSave save)
	{
		int highest = 0;
		foreach (int seed in save.SatchelSeeds)
		{
			highest = Math.Max(highest, seed);
		}
		foreach (int seed in save.WornSeeds)
		{
			highest = Math.Max(highest, seed);
		}
		return highest;
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
