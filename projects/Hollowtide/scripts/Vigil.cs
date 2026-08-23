using System;
using System.Collections.Generic;

namespace AetherGame;

/// <summary>What just happened, for anything that wants to say so out loud.</summary>
public enum Omen
{
	Plain,
	Good,
	Dread,
	Taken,
}

/// <summary>
/// The whole simulation: what the keeper owns, what it produces, how much dread it has
/// pulled in, and what the dark does about that.
/// </summary>
/// <remarks>
/// <para>
/// Deliberately free of any <c>AetherCore</c> reference. Time arrives as a delta, results
/// leave through <see cref="Announce"/>, and nothing here touches an entity - so the rules
/// of the game can be reasoned about, saved, and replayed for offline progress without a
/// frame, a canvas or a world existing. Every engine-facing script in this project is a
/// view onto this class.
/// </para>
/// <para>
/// Static rather than instanced because there is exactly one vigil per process, and
/// statics survive the editor Stop/Play cycle - so a session under test keeps its progress
/// across a hot reload instead of restarting the economy every time a script is rebuilt.
/// </para>
/// </remarks>
public static class Vigil
{
	// ── Persistent state ────────────────────────────────────────────────────────────

	public static string KeeperName = "Keeper";
	public static double Ichor;
	/// <summary>Everything gathered since the last communion. Drives the sigil payout.</summary>
	public static double RunIchor;
	/// <summary>Everything gathered ever, across communions. Drives offering unlocks.</summary>
	public static double LifetimeIchor;
	public static readonly int[] Owned = new int[Content.RiteCount];
	public static bool[] OfferingsTaken = new bool[Content.Offerings.Length];
	public static bool[] MarksEarned = new bool[Content.Marks.Length];
	/// <summary>Rites an overseer buys for you, bought with sigils and kept through communion.</summary>
	public static bool[] Overseers = new bool[Content.RiteCount];

	public static int Sigils;
	public static int Communions;

	/// <summary>0 to 1. The multiplier it pays and the thing that comes for you.</summary>
	public static double Dread;

	// ── Records, kept for the marks and the ledger ───────────────────────────────────

	public static double PlayedSeconds;
	public static double HighDreadSeconds;
	public static int WardsRaised;
	public static int TimesTaken;
	public static int CommunionSurges;
	public static int HandGathers;
	public static double SharedVigilSeconds;

	// ── Live state, not saved ────────────────────────────────────────────────────────

	/// <summary>Seconds left on a shared surge, and what it multiplies by while it lasts.</summary>
	public static double SurgeSeconds;
	public static double SurgeMultiplier = 1.0;

	/// <summary>Set by the session layer: 1.0 alone, more with other keepers present. Standing
	/// a vigil beside someone is worth something, which is the mechanical reason to host.</summary>
	public static double CongregationBonus = 1.0;

	/// <summary>Raised by the session layer while another keeper is connected.</summary>
	public static bool Alone = true;

	/// <summary>Called with anything worth saying to the player. The game layer routes it to
	/// the whisper feed; nothing in here knows that.</summary>
	public static Action<string, Omen>? Announce;

	/// <summary>Raised the moment a visitation takes something, so the presentation layer can
	/// react with more than a line of text.</summary>
	public static Action? OnVisitation;

	/// <summary>Bumped whenever a purchase, a loss or a communion changes what the ledger
	/// should show. Panels rebuild on a change rather than every frame.</summary>
	public static int Revision;

	// ── Derived values ───────────────────────────────────────────────────────────────

	/// <summary>What one more copy of a rite costs.</summary>
	public static double CostOf(int rite, int alreadyOwned)
		=> Content.Rites[rite].BaseCost * Math.Pow(Content.Rites[rite].Growth, alreadyOwned);

	/// <summary>What the next <paramref name="count"/> copies cost together - the geometric
	/// sum, not <paramref name="count"/> times the next one, which would undercharge badly
	/// at x100 and let the player buy a tier they cannot afford.</summary>
	public static double CostOfMany(int rite, int count)
	{
		if (count <= 0)
		{
			return 0.0;
		}
		double g = Content.Rites[rite].Growth;
		double first = CostOf(rite, Owned[rite]);
		return first * (Math.Pow(g, count) - 1.0) / (g - 1.0);
	}

	/// <summary>How many copies of a rite the purse covers right now.</summary>
	public static int Affordable(int rite)
	{
		double g = Content.Rites[rite].Growth;
		double first = CostOf(rite, Owned[rite]);
		if (Ichor < first)
		{
			return 0;
		}
		// Invert the geometric sum. Clamped because a purse deep enough to buy thousands
		// at once is a stutter, not a feature.
		double n = Math.Log(Ichor * (g - 1.0) / first + 1.0) / Math.Log(g);
		return Math.Clamp((int)Math.Floor(n), 0, 1000);
	}

	/// <summary>Free doublings from owning copies in lots of <see cref="Content.MilestoneStep"/>.</summary>
	public static double MilestoneMultiplier(int rite)
		=> Math.Pow(2.0, Owned[rite] / Content.MilestoneStep);

	/// <summary>Everything that multiplies one rite: its milestones, its offerings, and every
	/// global multiplier that also applies to it.</summary>
	public static double RiteMultiplier(int rite)
	{
		double mult = MilestoneMultiplier(rite);
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			if (OfferingsTaken[i] && Content.Offerings[i].Target == rite)
			{
				mult *= Content.Offerings[i].Multiplier;
			}
		}
		return mult * GlobalMultiplier;
	}

	/// <summary>Multipliers that apply to everything: global offerings, sigils, the dread
	/// bargain, a shared surge, and the congregation.</summary>
	public static double GlobalMultiplier
	{
		get
		{
			double mult = SigilMultiplier * DreadMultiplier * CongregationBonus;
			if (SurgeSeconds > 0.0)
			{
				mult *= SurgeMultiplier;
			}
			for (int i = 0; i < Content.Offerings.Length; i++)
			{
				if (OfferingsTaken[i] && Content.Offerings[i].Target == OfferingDef.TargetGlobal)
				{
					mult *= Content.Offerings[i].Multiplier;
				}
			}
			return mult;
		}
	}

	/// <summary>Each sigil is worth six percent of everything, forever.</summary>
	public static double SigilMultiplier => 1.0 + 0.06 * Sigils;

	/// <summary>The bargain at the centre of the game: dread pays, up to two and a half times
	/// at the brink. Every point of it is also what brings a visitation closer.</summary>
	public static double DreadMultiplier => 1.0 + 1.5 * Dread;

	/// <summary>Ichor per second from rites alone.</summary>
	public static double Rate
	{
		get
		{
			double total = 0.0;
			for (int i = 0; i < Content.RiteCount; i++)
			{
				if (Owned[i] > 0)
				{
					total += Owned[i] * Content.Rites[i].BaseRate * RiteMultiplier(i);
				}
			}
			return total;
		}
	}

	/// <summary>Dread per second at the current holdings. Wards do not reduce this - they cut
	/// the level, not the climb, so a deep parish is permanently harder to keep.</summary>
	public static double DreadRate
	{
		get
		{
			double total = 0.0;
			for (int i = 0; i < Content.RiteCount; i++)
			{
				total += Owned[i] * Content.Rites[i].DreadRate;
			}
			// A late run would otherwise pin the meter within seconds of a ward. Owning more
			// of a tier should raise dread; owning ALL the tiers should not make the game
			// unplayable, so the climb is compressed rather than summed flat.
			return Math.Sqrt(total) * 0.06;
		}
	}

	/// <summary>What one gather by hand is worth: a floor of one, plus a slice of the parish,
	/// so hand-gathering never stops being the thing you do while you wait.</summary>
	public static double HandGain
	{
		get
		{
			double hand = 1.0;
			for (int i = 0; i < Content.Offerings.Length; i++)
			{
				if (OfferingsTaken[i] && Content.Offerings[i].Target == OfferingDef.TargetHand)
				{
					hand *= Content.Offerings[i].Multiplier;
				}
			}
			return (hand + Rate * 0.05) * GlobalMultiplier;
		}
	}

	/// <summary>What raising a ward costs. Scales with the parish so it stays a real decision.</summary>
	public static double WardCost => 40.0 + Rate * 25.0;

	/// <summary>Sigils a communion would pay right now.</summary>
	public static int SigilsOnOffer => (int)Math.Floor(8.0 * Math.Sqrt(Math.Max(0.0, RunIchor) / 1e7));

	public static bool AnyOwnedAtLeast(int count)
	{
		for (int i = 0; i < Content.RiteCount; i++)
		{
			if (Owned[i] >= count)
			{
				return true;
			}
		}
		return false;
	}

	public static bool OwnsOneOfEach()
	{
		for (int i = 0; i < Content.RiteCount; i++)
		{
			if (Owned[i] < 1)
			{
				return false;
			}
		}
		return true;
	}

	/// <summary>An offering is shown once its condition is met; taken ones drop off the list.</summary>
	public static bool OfferingAvailable(int index)
	{
		if (OfferingsTaken[index])
		{
			return false;
		}
		OfferingDef def = Content.Offerings[index];
		return def.Target >= 0
			? Owned[def.Target] >= def.OwnedNeeded
			: LifetimeIchor >= def.LifetimeNeeded;
	}

	/// <summary>Highest tier with anything in it, or -1. What a visitation goes for, and where
	/// the world puts its camera.</summary>
	public static int DeepestRite()
	{
		for (int i = Content.RiteCount - 1; i >= 0; i--)
		{
			if (Owned[i] > 0)
			{
				return i;
			}
		}
		return -1;
	}

	// ── Actions ──────────────────────────────────────────────────────────────────────

	public static void Gather()
	{
		double gain = HandGain;
		Ichor += gain;
		RunIchor += gain;
		LifetimeIchor += gain;
		HandGathers++;
	}

	public static bool BuyRite(int rite, int count)
	{
		if (count <= 0)
		{
			return false;
		}
		double cost = CostOfMany(rite, count);
		if (cost > Ichor)
		{
			return false;
		}
		int before = Owned[rite] / Content.MilestoneStep;
		Ichor -= cost;
		Owned[rite] += count;
		Revision++;
		if (Owned[rite] / Content.MilestoneStep > before)
		{
			Say(Content.Rites[rite].Name + " doubles. The parish leans toward it.", Omen.Good);
		}
		return true;
	}

	public static bool TakeOffering(int index)
	{
		if (!OfferingAvailable(index) || Content.Offerings[index].Cost > Ichor)
		{
			return false;
		}
		Ichor -= Content.Offerings[index].Cost;
		OfferingsTaken[index] = true;
		Revision++;
		Say("Offered: " + Content.Offerings[index].Name, Omen.Good);
		return true;
	}

	/// <summary>Spend to push the dark back half a step.</summary>
	public static bool RaiseWard()
	{
		double cost = WardCost;
		if (cost > Ichor || Dread <= 0.0)
		{
			return false;
		}
		Ichor -= cost;
		Dread = Math.Max(0.0, Dread - 0.5);
		WardsRaised++;
		Revision++;
		Say("A ward goes up. Whatever was closing stops where it is.", Omen.Good);
		return true;
	}

	/// <summary>An overseer buys one rite for you forever, paid for in sigils.</summary>
	public static int OverseerCost(int rite) => 3 * (rite + 1);

	public static bool HireOverseer(int rite)
	{
		if (Overseers[rite] || Sigils < OverseerCost(rite))
		{
			return false;
		}
		Sigils -= OverseerCost(rite);
		Overseers[rite] = true;
		Revision++;
		Say("An overseer takes the " + Content.Rites[rite].Name + ". It does not look up.", Omen.Plain);
		return true;
	}

	/// <summary>Give the parish back. Keeps sigils, overseers, marks and the name; loses
	/// everything else.</summary>
	public static bool Commune()
	{
		int payout = SigilsOnOffer;
		if (payout <= 0)
		{
			return false;
		}
		Sigils += payout;
		Communions++;
		Ichor = 0.0;
		RunIchor = 0.0;
		Dread = 0.0;
		SurgeSeconds = 0.0;
		Array.Clear(Owned, 0, Owned.Length);
		Array.Clear(OfferingsTaken, 0, OfferingsTaken.Length);
		Revision++;
		Say("Communion. You wake with " + payout + " more sigils and none of the parish.", Omen.Good);
		return true;
	}

	/// <summary>A shared surge, from the bell or from another keeper.</summary>
	public static void BeginSurge(double seconds, double multiplier, bool fromCongregation)
	{
		// Longest wins rather than stacking: two keepers ringing at once should feel like one
		// louder bell, not a multiplied economy.
		if (seconds > SurgeSeconds)
		{
			SurgeSeconds = seconds;
		}
		SurgeMultiplier = Math.Max(SurgeMultiplier, multiplier);
		if (fromCongregation)
		{
			CommunionSurges++;
		}
	}

	/// <summary>Ichor arriving from another keeper.</summary>
	public static void ReceiveTithe(double amount, string from)
	{
		if (amount <= 0.0)
		{
			return;
		}
		Ichor += amount;
		RunIchor += amount;
		LifetimeIchor += amount;
		Say(from + " tithes you " + Numbers.Short(amount) + " ichor.", Omen.Good);
	}

	/// <summary>Dread arriving from another keeper, who no longer has it.</summary>
	public static void ReceiveDread(double amount, string from)
	{
		if (amount <= 0.0)
		{
			return;
		}
		Dread = Math.Clamp(Dread + amount, 0.0, 1.0);
		Say(from + " turns something loose. It comes to you instead.", Omen.Dread);
	}

	/// <summary>Push dread onto someone else. Returns what actually left, so the caller only
	/// sends what it really gave up.</summary>
	public static double ShedDread(double amount)
	{
		double shed = Math.Min(Dread, amount);
		Dread -= shed;
		return shed;
	}

	public static bool SpendForTithe(double amount)
	{
		if (amount <= 0.0 || amount > Ichor)
		{
			return false;
		}
		Ichor -= amount;
		return true;
	}

	// ── The tick ─────────────────────────────────────────────────────────────────────

	/// <summary>Advance the whole simulation. <paramref name="offline"/> runs the same rules at
	/// reduced yield and without visitations, so returning to a save cannot cost you a rite
	/// you were not there to defend.</summary>
	public static void Tick(double deltaSeconds, bool offline = false)
	{
		if (deltaSeconds <= 0.0)
		{
			return;
		}

		if (SurgeSeconds > 0.0)
		{
			SurgeSeconds -= deltaSeconds;
			if (SurgeSeconds <= 0.0)
			{
				SurgeSeconds = 0.0;
				SurgeMultiplier = 1.0;
			}
		}

		double efficiency = offline ? 0.5 : 1.0;
		double gained = Rate * deltaSeconds * efficiency;
		Ichor += gained;
		RunIchor += gained;
		LifetimeIchor += gained;
		PlayedSeconds += deltaSeconds;

		Dread = Math.Clamp(Dread + DreadRate * deltaSeconds * efficiency, 0.0, 1.0);
		if (Dread >= 0.75)
		{
			HighDreadSeconds += deltaSeconds;
		}
		if (!Alone)
		{
			SharedVigilSeconds += deltaSeconds;
		}

		RunOverseers();

		if (!offline && Dread >= 1.0)
		{
			Visitation();
		}

		CheckMarks();
	}

	/// <summary>Overseers buy one copy at a time and only out of surplus, so automation never
	/// spends the ward money that is keeping the lights on.</summary>
	private static void RunOverseers()
	{
		double reserve = WardCost;
		for (int i = Content.RiteCount - 1; i >= 0; i--)
		{
			if (!Overseers[i])
			{
				continue;
			}
			double cost = CostOf(i, Owned[i]);
			if (Ichor - cost >= reserve)
			{
				Ichor -= cost;
				Owned[i]++;
				Revision++;
			}
		}
	}

	/// <summary>The meter filled. Something takes a share of the deepest thing you own and
	/// leaves the dread most of the way down, so a visitation resets the bargain rather than
	/// ending the run.</summary>
	private static void Visitation()
	{
		int rite = DeepestRite();
		Dread = 0.35;
		TimesTaken++;
		Revision++;

		if (rite < 0)
		{
			Say("Something walks the empty parish and finds nothing to take.", Omen.Dread);
			OnVisitation?.Invoke();
			return;
		}

		int taken = Math.Max(1, (int)Math.Ceiling(Owned[rite] * 0.15));
		Owned[rite] = Math.Max(0, Owned[rite] - taken);
		Say(Content.Rites[rite].TakenLine, Omen.Taken);
		OnVisitation?.Invoke();
	}

	private static void CheckMarks()
	{
		for (int i = 0; i < Content.Marks.Length; i++)
		{
			if (!MarksEarned[i] && Content.Marks[i].Earned())
			{
				MarksEarned[i] = true;
				Say("Marked: " + Content.Marks[i].Name, Omen.Good);
			}
		}
	}

	public static int MarksHeld()
	{
		int held = 0;
		for (int i = 0; i < MarksEarned.Length; i++)
		{
			if (MarksEarned[i])
			{
				held++;
			}
		}
		return held;
	}

	/// <summary>Replay the time a save was closed for. Chunked rather than applied in one step
	/// because production compounds through overseers - a single huge delta would buy nothing
	/// and under-pay a parish that would have been growing the whole time.</summary>
	public static OfflineReport CatchUp(double seconds)
	{
		const double kCapSeconds = 8.0 * 3600.0;
		double clamped = Math.Clamp(seconds, 0.0, kCapSeconds);
		double before = Ichor;
		double dreadBefore = Dread;

		const int kSteps = 60;
		double step = clamped / kSteps;
		for (int i = 0; i < kSteps && step > 0.0; i++)
		{
			Tick(step, offline: true);
		}

		return new OfflineReport
		{
			Seconds = clamped,
			Capped = seconds > kCapSeconds,
			Ichor = Ichor - before,
			Dread = Dread - dreadBefore,
		};
	}

	private static void Say(string line, Omen omen) => Announce?.Invoke(line, omen);

	/// <summary>Wipe live state back to a fresh vigil. Used by a new game and by the tests.</summary>
	public static void Reset()
	{
		Ichor = 0.0;
		RunIchor = 0.0;
		LifetimeIchor = 0.0;
		Array.Clear(Owned, 0, Owned.Length);
		OfferingsTaken = new bool[Content.Offerings.Length];
		MarksEarned = new bool[Content.Marks.Length];
		Overseers = new bool[Content.RiteCount];
		Sigils = 0;
		Communions = 0;
		Dread = 0.0;
		PlayedSeconds = 0.0;
		HighDreadSeconds = 0.0;
		WardsRaised = 0;
		TimesTaken = 0;
		CommunionSurges = 0;
		HandGathers = 0;
		SharedVigilSeconds = 0.0;
		SurgeSeconds = 0.0;
		SurgeMultiplier = 1.0;
		Revision++;
	}
}

/// <summary>What the keeper missed while the game was closed.</summary>
public struct OfflineReport
{
	public double Seconds;
	public bool Capped;
	public double Ichor;
	public double Dread;
}
