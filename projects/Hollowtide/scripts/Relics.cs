using System;

namespace AetherGame;

/// <summary>
/// How far the dark has got into a thing. Ascending, and the ladder is the parish's own
/// story: ordinary leavings, something somebody kept, something consecrated, something
/// hallowed - and then the word this game is named for, which is what hallowed becomes
/// when it has been eaten.
/// </summary>
public enum Grade
{
	Leavings,
	Keepsake,
	Anointed,
	Hallowed,
	Hollowed,
}

/// <summary>What a relic does. Every one of these turns a knob the game already had, for
/// the same reason the boons do: a relic should deepen the vigil you know how to keep, not
/// hand you a second game to learn alongside it.</summary>
public enum Power
{
	/// <summary>Gathering by hand takes more.</summary>
	Hand,
	/// <summary>Dread pays better.</summary>
	Bargain,
	/// <summary>Wards cost less.</summary>
	Warding,
	/// <summary>Longer to answer what is walking.</summary>
	Patience,
	/// <summary>Offerings cost less.</summary>
	Almsgiving,
	/// <summary>Fervour leaves you more slowly.</summary>
	Steadiness,
}

/// <summary>
/// One thing dug out of the parish.
/// </summary>
/// <remarks>
/// A relic is a SEED and a grade, and everything else about it - its name, its art, what it
/// does and how much - is derived from those two. That is what makes the supply endless
/// without a table of hand-authored items to maintain, and it is why two keepers who find
/// the same seed find the same relic.
/// </remarks>
public struct Relic
{
	public int Seed;
	public Grade Grade;

	public bool Exists => Seed != 0;
}

/// <summary>
/// Everything about relics that is a pure function of a seed.
/// </summary>
/// <remarks>
/// <para>
/// Deliberately free of state. Generation has to be reproducible from the seed alone - the
/// save stores two numbers per relic, the art is drawn from the same seed on the GPU, and a
/// traded relic has to be the same object in both keepers' hands. Anything that read live
/// state here would break all three at once.
/// </para>
/// <para>
/// The parish is what you are digging through, so the vocabulary is a parish's: what things
/// are made of, what shape they were left in, and whose they were.
/// </para>
/// </remarks>
public static class Relics
{
	/// <summary>How many relics a keeper can carry at once. Small, because choosing what to
	/// leave behind is the interesting half of finding things.</summary>
	public const int Slots = 3;

	/// <summary>How much of a satchel a keeper has before they must throw something away.</summary>
	public const int Satchel = 12;

	private static readonly string[] s_material =
	{
		"Bone", "Wax", "Tallow", "Iron", "Salt", "Ash", "Chalk", "Gilt", "Glass", "Hair",
		"Lead", "Amber", "Pitch", "Linen", "Char", "Silver", "Rushlight", "Grave-Iron",
		"Coffin-Oak", "Quicklime", "Widow's-Glass", "Bell-Bronze", "Marrow", "Nettle",
		"Slate", "Tar", "Ivory", "Sackcloth",
	};

	private static readonly string[] s_form =
	{
		"Charm", "Nail", "Bead", "Thimble", "Key", "Tooth", "Ring", "Cord", "Splinter",
		"Lens", "Bell", "Pin", "Knot", "Coin", "Hook", "Vial", "Stopper", "Hasp",
		"Reliquary", "Censer", "Tally", "Sleeve", "Spool", "Whistle", "Latch", "Locket",
		"Buckle", "Shroud-Pin",
	};

	/// <summary>Where it came from. Only the better grades earn one - a common thing is just
	/// a thing, and giving everything a provenance makes provenance worthless.</summary>
	private static readonly string[] s_provenance =
	{
		"of the Ninth Night", "of the Drowned Chapel", "the Choir Kept", "from under the Flags",
		"of the Long Hour", "the Shepherd Counted", "of the Unlit Aisle", "left in the Ossuary",
		"of the Third Rendering", "the Loom Wove", "of the Tide-Mark", "no one Claimed",
		"of the Sealed Vestry", "the Lanterns Missed", "from the Low Aisle", "of the Quiet Bell",
		"the Mouth Returned", "of the Salt Line", "the Statue Faced", "from the Second Grave",
	};

	/// <summary>
	/// What rendering a relic down is worth, in seconds of the parish's production.
	/// </summary>
	/// <remarks>
	/// Leaving a relic behind used to simply delete it, which is a poor thing to ask of anybody
	/// holding something they went and found. It renders down instead - the parish already has
	/// the word for it, in the Third Rendering - and pays out in ichor.
	///
	/// Sized as TIDYING, not as an income. Relics come out of clicking, so if rendering paid
	/// well the loop would become click-render-repeat and quietly replace the parish it is
	/// meant to sit beside. Doubling per grade means junk is worth clearing and a Hollowed
	/// thing is still worth far more worn than melted.
	///
	/// The first sizing was eight times this, and a keeper who rendered everything the instant
	/// it landed took a QUARTER of their whole income that way and finished three times ahead
	/// of one who did not. That is not a bonus for tidying, it is the game.
	/// </remarks>
	public static double RenderSeconds(Grade grade) => 0.05 * Math.Pow(2.0, (int)grade);

	/// <summary>Said of the very best. One line, and only Hollowed things get one.</summary>
	private static readonly string[] s_epithet =
	{
		"Unquiet", "Unaccounted", "Still Warm", "Answering", "Half-Awake", "Remembering",
		"Unfinished", "Listening", "Patient", "Wrongly Named",
	};

	/// <summary>The size of each vocabulary, so anything checking how much the parish has to
	/// give can reason about the SPACE rather than sample it and guess.</summary>
	public static int Materials => s_material.Length;
	public static int Forms => s_form.Length;
	public static int Provenances => s_provenance.Length;
	public static int Epithets => s_epithet.Length;

	public static string GradeName(Grade grade) => grade switch
	{
		Grade.Leavings => "Leavings",
		Grade.Keepsake => "Keepsake",
		Grade.Anointed => "Anointed",
		Grade.Hallowed => "Hallowed",
		_ => "Hollowed",
	};

	/// <summary>
	/// A deterministic, well-mixed hash of a seed and a channel.
	/// </summary>
	/// <remarks>
	/// Every derived property draws from its own channel rather than from a shared running
	/// generator, so adding a property later cannot shift the ones already rolled - a save
	/// full of relics has to keep meaning what it meant. This is also why the art can be
	/// drawn on the GPU from the same seed without the two ever comparing notes.
	/// </remarks>
	public static uint Hash(int seed, int channel)
	{
		unchecked
		{
			uint h = (uint)seed * 2654435761u ^ (uint)channel * 2246822519u;
			h ^= h >> 15;
			h *= 2246822519u;
			h ^= h >> 13;
			h *= 3266489917u;
			h ^= h >> 16;
			return h;
		}
	}

	private static int Pick(int seed, int channel, int count) => (int)(Hash(seed, channel) % (uint)count);

	/// <summary>
	/// The seed as the art shader has to receive it.
	/// </summary>
	/// <remarks>
	/// Material parameters travel as a float4, and float32 holds integers exactly only up to
	/// 16,777,216. Relic seeds start near a billion and count up by one, so passing the seed
	/// itself put runs of SIXTY-FIVE consecutive relics onto the same float - sixty-five relics
	/// with different names, different powers and identical pictures. Hashed down to a range a
	/// float carries exactly, every relic gets its own drawing again.
	/// </remarks>
	public static float ArtSeed(Relic relic) => Hash(relic.Seed, 40) % 65536u;

	/// <summary>The name, built from the seed. Longer and stranger the better the grade, so a
	/// keeper can tell roughly what they are holding before reading a single number.</summary>
	public static string NameOf(Relic relic)
	{
		string body = s_material[Pick(relic.Seed, 1, s_material.Length)] + " " +
			s_form[Pick(relic.Seed, 2, s_form.Length)];
		if (relic.Grade >= Grade.Anointed)
		{
			body += " " + s_provenance[Pick(relic.Seed, 3, s_provenance.Length)];
		}
		if (relic.Grade == Grade.Hollowed)
		{
			body = "The " + s_epithet[Pick(relic.Seed, 4, s_epithet.Length)] + " " + body;
		}
		return body;
	}

	/// <summary>How many powers a relic of this grade carries. The whole reason a grade is
	/// worth wanting: rarity is not a bigger number on the same line, it is more lines.</summary>
	public static int PowerCount(Grade grade) => grade switch
	{
		Grade.Leavings => 1,
		Grade.Keepsake => 1,
		Grade.Anointed => 2,
		Grade.Hallowed => 2,
		_ => 3,
	};

	/// <summary>The powers a relic carries, in order. Distinct by construction: a relic with
	/// the same power twice reads as a bug however it is presented.</summary>
	public static Power PowerAt(Relic relic, int index)
	{
		// Drawn WITHOUT REPLACEMENT, one power at a time out of a shrinking pool. The first
		// version walked forward by a stride and assumed the stride would be co-prime with the
		// number of powers - it is not, for six powers a stride of 2, 3 or 4 wraps onto a power
		// already taken, and a relic granting the same thing twice reads as a bug however it is
		// presented. This cannot collide for any number of powers anybody adds later.
		int count = Enum.GetValues<Power>().Length;
		Span<int> pool = stackalloc int[count];
		for (int i = 0; i < count; i++)
		{
			pool[i] = i;
		}

		int remaining = count;
		int chosen = 0;
		for (int step = 0; step <= index && remaining > 0; step++)
		{
			int pick = (int)(Hash(relic.Seed, 10 + step) % (uint)remaining);
			chosen = pool[pick];
			pool[pick] = pool[--remaining];
		}
		return (Power)chosen;
	}

	/// <summary>
	/// How strong one of a relic's powers is, as a fraction.
	/// </summary>
	/// <remarks>
	/// Scales with the grade and varies within it, so two Hallowed relics are not the same
	/// relic - but the bands do not overlap far, so a lucky Keepsake never outclasses an
	/// unlucky Hallowed. Rarity has to mean something at a glance or it is decoration.
	/// </remarks>
	public static double MagnitudeAt(Relic relic, int index)
	{
		// Halved from the first pass, which put a full loadout at roughly ten times a bare
		// keeper over an hour. Measured against everything else in the game - the echoes at
		// 1.24x, answering visitors at 1.45x, stoking at 4.9x - that made relics the whole
		// game and the rest of it decoration. They should be the best single lever a keeper
		// has and still be in the same conversation as the others.
		double floor = 0.02 + 0.025 * (int)relic.Grade;
		double spread = 0.01 + 0.01 * (int)relic.Grade;
		return floor + spread * (Hash(relic.Seed, 20 + index) % 1000u) / 1000.0;
	}

	/// <summary>What one power reads as on the page.</summary>
	public static string Describe(Power power, double magnitude)
	{
		string amount = "+" + Numbers.Percent(magnitude);
		return power switch
		{
			Power.Hand => amount + " by hand",
			Power.Bargain => amount + " from dread",
			Power.Warding => Numbers.Percent(magnitude) + " off wards",
			Power.Patience => amount + " longer to answer",
			Power.Almsgiving => Numbers.Percent(magnitude) + " off offerings",
			_ => Numbers.Percent(magnitude) + " slower to lose fervour",
		};
	}

	/// <summary>
	/// Roll what a keeper turns up, given how deep in the dark they were when they dug.
	/// </summary>
	/// <remarks>
	/// <para>
	/// <b>Grade is a function of dread.</b> This is the whole reason the system belongs in
	/// this game rather than beside it: hand-gathering at the brink turns up better things
	/// than hand-gathering in safety, so the reason to click and the reason to ride the meter
	/// are the same reason. A loot table rolled off a flat chance would have been a second
	/// game running in parallel with the first.
	/// </para>
	/// <para>
	/// Returns a relic with a zero seed when nothing was found, which is most of the time.
	/// </para>
	/// </remarks>
	public static Relic Dig(Random rng, double dread, int roll)
	{
		// Roughly one in sixty clicks in safety, one in fifteen at the brink.
		double chance = 0.017 + 0.05 * dread;
		if (rng.NextDouble() > chance)
		{
			return default;
		}

		// Each grade needs the dread that justifies it AND a roll on top, so the brink makes
		// good things possible rather than guaranteed.
		//
		// The TOP band is the one to watch, because it is open-ended: everything above the last
		// threshold is Hollowed, so if the roll can reach much past it the rarest grade quietly
		// becomes the commonest of the good ones. Measured at the brink under the first numbers,
		// Hollowed came up 22.2% against Hallowed's 17.2% and Anointed's 17.8% - a ladder that
		// inverted exactly where a keeper spends their most dangerous minutes. The range now
		// runs further than the last threshold by less than the bands below it are wide.
		double luck = rng.NextDouble() * (0.35 + dread * 1.15);
		Grade grade = luck switch
		{
			> 1.30 => Grade.Hollowed,
			> 0.95 => Grade.Hallowed,
			> 0.62 => Grade.Anointed,
			> 0.30 => Grade.Keepsake,
			_ => Grade.Leavings,
		};

		// Never zero: a zero seed is how "nothing" is spelled.
		int seed = roll == 0 ? 1 : roll;
		return new Relic { Seed = seed, Grade = grade };
	}
}
