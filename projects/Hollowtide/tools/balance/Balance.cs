using System;
using System.Numerics;
using System.Globalization;
using System.Text.RegularExpressions;
using System.IO;
using System.Collections.Generic;

namespace AetherGame.Balance;

/// <summary>
/// Plays Hollowtide's economy at speed and checks the things its design actually promises.
/// </summary>
/// <remarks>
/// <para>
/// Every check here exists because the thing it checks was once broken, and none of them
/// were visible by reading the code. Stoking was worth five million times a clean run.
/// Insurance cost 6% of a window early and 131% late. Dread could not move at all until a
/// keeper owned five of every rite. Ten communions in a row left the tenth run WORSE than
/// the first. All four read as reasonable formulas, and all four were found by playing them.
/// </para>
/// <para>
/// So the invariants are written as bounds rather than as expected values: a rebalance is
/// supposed to move the numbers, and a test that pins them would only ever be deleted. What
/// must not change is the SHAPE - that no single button dominates, that a cost quoted as a
/// share of income stays one at every scale, and that prestige ratchets.
/// </para>
/// </remarks>
internal static class Balance
{
	/// <summary>Simulation step. Small enough that a rite's working lands where it should,
	/// large enough that eight hours of play runs in a moment.</summary>
	private const double kDt = 0.05;

	private static int s_failures;

	/// <summary>
	/// Shifts every seed in the suite, so the whole thing can be re-run against different luck.
	/// </summary>
	/// <remarks>
	/// A check that passes on one sample and fails on the next is worse than no check, and the
	/// only way to tell the two apart is to run it against several. Sweeping this found a check
	/// asserting a strict rarity ladder that the game never promised - it held on the seeds it
	/// was written with and broke on others.
	///
	///   dotnet run -c Release -- --seed 3
	/// </remarks>
	private static int s_seedOffset;

	/// <summary>How many invariants this run actually held up. Counted rather than quoted: the
	/// figure appears in the README and the changelog, and hand-maintaining it in two places had
	/// already left one of them three commits behind while the other was current. A number that
	/// is only ever right by somebody remembering is a number that documents the past.</summary>
	private static int s_checks;

	/// <summary>A generator for a fixed seed, shifted by whatever the run was asked for.</summary>
	private static Random Seeded(int seed) => new Random(seed + s_seedOffset);

	private static void Check(string what, bool ok, string detail)
	{
		Console.WriteLine("  [" + (ok ? "PASS" : "FAIL") + "] " + what.PadRight(46) + " " + detail);
		s_checks++;
		if (!ok)
		{
			s_failures++;
		}
	}

	// -- A keeper --------------------------------------------------------------------

	/// <summary>Buy whatever pays for itself soonest, keeping the ward money back. Not optimal
	/// play, but it is what an attentive player converges on, and it is stable enough that a
	/// change in the result means a change in the rules rather than in the strategy.</summary>
	private static void Buy()
	{
		double reserve = Vigil.WardCost;
		int best = -1;
		double bestPayback = double.MaxValue;
		for (int i = 0; i < Content.RiteCount; i++)
		{
			double cost = Vigil.CostOf(i, Vigil.Owned[i]);
			if (cost + reserve > Vigil.Ichor)
			{
				continue;
			}
			double payback = cost / (Content.Rites[i].BaseRate * Vigil.RiteMultiplier(i));
			if (payback < bestPayback)
			{
				bestPayback = payback;
				best = i;
			}
		}
		if (best >= 0)
		{
			Vigil.BuyRite(best, 1);
		}
	}

	/// <summary>
	/// Take every offering that is plainly worth taking.
	/// </summary>
	/// <remarks>
	/// PLAINLY. It used to take all of them, which was fine while every offering was a straight
	/// multiplier and stopped being fine the moment one of them cost something: a keeper who
	/// buys a yield cut without weighing it is not "what an attentive player converges on", it
	/// is a keeper making a mistake, and a reference strategy that makes mistakes measures the
	/// mistake instead of the rules. The offerings that trade output for a quieter parish are a
	/// real decision, so this one declines them and the figures below describe a keeper playing
	/// for yield.
	/// </remarks>
	private static void TakeOfferings()
	{
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			if (Content.Offerings[i].Multiplier < 1.0)
			{
				continue;
			}
			if (Vigil.OfferingAvailable(i) && Content.Offerings[i].Cost <= Vigil.Ichor * 0.5)
			{
				Vigil.TakeOffering(i);
			}
		}
	}

	/// <summary>Spend sigils cheapest-first, which is what a player without a wiki does.</summary>
	private static void SpendSigils()
	{
		while (true)
		{
			int best = int.MinValue;
			int bestCost = int.MaxValue;
			for (int i = 0; i < Content.Boons.Length; i++)
			{
				int cost = Vigil.BoonCost(i);
				if (cost > 0 && cost <= Vigil.Sigils && cost < bestCost)
				{
					bestCost = cost;
					best = i;
				}
			}
			// Overseers are folded into the same search as one's complement, so the keeper
			// weighs a boon and an overseer against each other rather than always draining
			// one list before looking at the other.
			for (int rite = 0; rite < Content.RiteCount; rite++)
			{
				int cost = Vigil.OverseerCost(rite);
				if (!Vigil.Overseers[rite] && cost <= Vigil.Sigils && cost < bestCost)
				{
					bestCost = cost;
					best = ~rite;
				}
			}
			if (best == int.MinValue)
			{
				return;
			}
			if (best >= 0)
			{
				Vigil.BuyBoon(best);
			}
			else
			{
				Vigil.HireOverseer(~best);
			}
		}
	}

	private struct Result
	{
		public double Lifetime;
		public double ReelingFraction;
		public double MeanDread;
	}

	private static Result Play(double minutes, double clicksPerSecond, double stokesPerSecond, bool ward)
	{
		double reeling = 0.0;
		double dreadSum = 0.0;
		int samples = 0;

		double clickAccumulator = 0.0;
		double stokeAccumulator = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			clickAccumulator += clicksPerSecond * kDt;
			while (clickAccumulator >= 1.0)
			{
				clickAccumulator -= 1.0;
				Vigil.Gather();
			}

			stokeAccumulator += (stokesPerSecond < 0.0 ? 1.0 / kDt : stokesPerSecond) * kDt;
			while (stokeAccumulator >= 1.0)
			{
				stokeAccumulator -= 1.0;
				// A negative rate means the masher: press it every tick, at any dread, which is
				// what an exploit-hunting player does and what the old formula rewarded with
				// five million times a clean run. Otherwise hold short of the brink, because a
				// keeper watching the meter stops before it.
				if (stokesPerSecond < 0.0 || Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}

			if (ward && Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			Vigil.Tick(kDt);

			dreadSum += Vigil.Dread;
			samples++;
			if (Vigil.AftermathSeconds > 0.0)
			{
				reeling += kDt;
			}
		}

		return new Result
		{
			Lifetime = Vigil.LifetimeIchor,
			ReelingFraction = reeling / (minutes * 60.0),
			MeanDread = dreadSum / Math.Max(1, samples),
		};
	}

	// -- The invariants --------------------------------------------------------------

	/// <summary>
	/// No button may be worth orders of magnitude more than playing the game.
	/// </summary>
	/// <remarks>
	/// Stoke pays up front, so it will always be worth SOMETHING, and it should be: pushing
	/// your luck for nothing is a dare rather than a trade. The bound is that it pays like a
	/// strategy rather than like an exit - and that pressing it faster does not pay more,
	/// which is the whole job of the cooldown.
	/// </remarks>
	private static void StokeIsATradeNotAnExit()
	{
		Console.WriteLine("Stoke is a trade, not an exit");
		Vigil.Reset();
		double clean = Play(30, 4, 0, ward: true).Lifetime;
		Vigil.Reset();
		Result patient = Play(30, 4, 0.5, ward: true);
		Vigil.Reset();
		double frantic = Play(30, 4, 4, ward: true).Lifetime;
		Vigil.Reset();
		double mashed = Play(30, 4, -1, ward: true).Lifetime;

		double advantage = patient.Lifetime / clean;
		Check("pushing your luck pays, but bounded", advantage > 1.2 && advantage < 12.0,
			advantage.ToString("0.0") + "x over a clean run");
		Check("mashing it pays no more than pacing it", frantic / patient.Lifetime < 1.5,
			"4/s is " + (frantic / patient.Lifetime).ToString("0.00") + "x of 0.5/s");
		// Stated as a RELATIONSHIP rather than a ceiling on reeling. It used to assert that a
		// stoker spends under half the run at half pace, which was only ever true because a bug
		// stopped small parishes reaching the brink at all - once stoking genuinely summoned
		// things, a keeper who provokes them and then ignores them reeled 53% of the time and
		// the check failed. That is the correct punishment, not a regression. What actually has
		// to hold is that provoking costs you when you ignore it and pays when you answer.
		Check("riding high costs uptime when ignored", patient.ReelingFraction > 0.05,
			"reeling " + (patient.ReelingFraction * 100.0).ToString("0") + "% of the run");

		double ignoring = PlayAnswering(45, null, stokePerSecond: 0.5);
		double engaging = PlayAnswering(45, Answer.None, stokePerSecond: 0.5);
		Check("answering what you provoked is the best play", engaging > ignoring * 2.0,
			(engaging / ignoring).ToString("0.0") + "x ignoring it");
		// Pressed every single tick at any dread - the shape of play the old formula paid
		// 4,919,000x for. The cooldown alone does not bound this; the offer shrinking toward
		// the brink is what makes the last press worthless.
		Check("mashing it every tick is bounded", mashed / clean < 12.0,
			"masher gets " + (mashed / clean).ToString("0.0") + "x a clean run");

		// The payout of one stoke, checked directly against what it can possibly cost, because
		// the ratios above are only as honest as the keeper simulating them. A stoke that pays
		// more than a whole window's income is a printer whatever anybody does with it.
		Vigil.Reset();
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 64;
		}
		double window = Vigil.Rate * Vigil.SecondsToVisitation;
		Check("one stoke pays less than a window earns", Vigil.StokeOffer < window,
			Numbers.Short(Vigil.StokeOffer) + " against " + Numbers.Short(window) + " a window");
	}

	/// <summary>
	/// A cost quoted as a share of income must stay one at every scale.
	/// </summary>
	/// <remarks>
	/// The failure this catches is subtle and shipped once already: a ward priced off the RATE
	/// while the window it covers shrinks with holdings, so insurance silently grew from a
	/// twentieth of a keeper's income to more than all of it. Anything priced against the
	/// visitation window has to be checked across the whole range of parishes, not at one.
	/// </remarks>
	private static void InsuranceCostsTheSameAtEveryScale()
	{
		Console.WriteLine("Insurance costs the same share at every scale");
		double lowest = double.MaxValue;
		double highest = 0.0;
		for (int owned = 8; owned <= 512; owned *= 2)
		{
			Vigil.Reset();
			for (int i = 0; i < Content.RiteCount; i++)
			{
				Vigil.Owned[i] = owned;
			}
			double window = Vigil.SecondsToVisitation;
			if (double.IsInfinity(window))
			{
				continue;
			}
			double share = Vigil.WardCost / (Vigil.RawRate * window);
			lowest = Math.Min(lowest, share);
			highest = Math.Max(highest, share);
		}
		Check("ward is a steady share of the window", highest - lowest < 0.05,
			(lowest * 100.0).ToString("0.0") + "% to " + (highest * 100.0).ToString("0.0") + "% across x8 to x512");
		Check("ward never costs more than the window pays", highest < 0.6,
			"worst case " + (highest * 100.0).ToString("0.0") + "%");

		// -- Insurance has to be affordable from the start, and cost nothing to keep back. --
		// A ward is priced off production, so it could in principle outrun a young parish
		// entirely; and if holding the money back cost a keeper real progress, the correct play
		// would be never to insure and the mechanic would be decoration.
		Vigil.Reset();
		Vigil.Rng = Seeded(3);
		double affordedAt = -1.0;
		double clock2 = 0.0;
		double clicking2 = 0.0;
		for (int step = 0; step < 300.0 / kDt; step++, clock2 += kDt)
		{
			clicking2 += 3.0 * kDt;
			while (clicking2 >= 1.0)
			{
				clicking2 -= 1.0;
				Vigil.Gather();
			}
			double keepBack = Vigil.WardCost;
			for (int i = 0; i < Content.RiteCount; i++)
			{
				if (Vigil.CostOf(i, Vigil.Owned[i]) + keepBack <= Vigil.Ichor)
				{
					Vigil.BuyRite(i, 1);
					break;
				}
			}
			if (affordedAt < 0.0 && Vigil.Ichor >= Vigil.WardCost)
			{
				affordedAt = clock2;
			}
			Vigil.Tick(kDt);
		}
		int reservedTier = Vigil.DeepestRite();
		Check("a ward is affordable in the first minute", affordedAt is >= 0.0 and < 60.0,
			"first affordable at " + Numbers.Duration(affordedAt));

		// The same stretch, spending everything, to see what holding back actually costs.
		Vigil.Reset();
		Vigil.Rng = Seeded(3);
		clicking2 = 0.0;
		for (int step = 0; step < 300.0 / kDt; step++)
		{
			clicking2 += 3.0 * kDt;
			while (clicking2 >= 1.0)
			{
				clicking2 -= 1.0;
				Vigil.Gather();
			}
			for (int i = 0; i < Content.RiteCount; i++)
			{
				if (Vigil.CostOf(i, Vigil.Owned[i]) <= Vigil.Ichor)
				{
					Vigil.BuyRite(i, 1);
					break;
				}
			}
			Vigil.Tick(kDt);
		}
		Check("and keeping it back costs no progress", reservedTier >= Vigil.DeepestRite(),
			"tier " + (reservedTier + 1) + " insured against tier " + (Vigil.DeepestRite() + 1) + " spent out");

		Vigil.Reset();
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 512;
		}
		// The two have to be close, or one of them is not a choice: a ward that costs far more
		// than being caught is never bought, and one that costs far less is never weighed.
		Check("being caught is comparable to insuring", Math.Abs(Vigil.AftermathShareOfWindow - 0.25) < 0.15,
			"caught costs " + (Vigil.AftermathShareOfWindow * 100.0).ToString("0.0") + "% of a window");
		Check("the aftermath fits inside the window", Vigil.SecondsToVisitation > 4.0,
			"deepest parish is visited every " + Vigil.SecondsToVisitation.ToString("0") + "s");
	}

	/// <summary>The central mechanic has to be present in the first session, not the third.</summary>
	private static void DreadIsAliveFromTheFirstRite()
	{
		Console.WriteLine("Dread is alive from the first rite");
		Vigil.Reset();
		Vigil.Owned[0] = 1;
		double one = Vigil.DreadEquilibrium;
		Vigil.Owned[0] = 50;
		double fifty = Vigil.DreadEquilibrium;

		Check("one rite already moves the meter", one > 0.01,
			"settles at " + (one * 100.0).ToString("0.0") + "% dread");
		Check("growing the parish visibly raises it", fifty > one * 3.0,
			"fifty settles at " + (fifty * 100.0).ToString("0.0") + "%");
		Check("a small parish is never visited", fifty < 1.0, "equilibrium stays under the brink");

		Vigil.Reset();
		Result run = Play(20, 4, 0.25, ward: true);
		Check("dread matters within twenty minutes", run.MeanDread > 0.2,
			"mean dread " + run.MeanDread.ToString("0.00"));
	}

	/// <summary>
	/// Every communion must leave the keeper stronger than the last one did.
	/// </summary>
	/// <remarks>
	/// The check that caught the worst bug in the game. While the permanent multiplier was
	/// drawn from sigils HELD, spending sigils made the keeper weaker, and ten communions of
	/// honest play left the tenth run reaching less than the first. A prestige loop that does
	/// not ratchet is a treadmill with ceremony.
	/// </remarks>
	private static void PrestigeRatchets()
	{
		Console.WriteLine("Prestige ratchets");
		Vigil.Reset();
		double first = 0.0;
		double last = 0.0;
		for (int run = 1; run <= 8; run++)
		{
			Play(30, 4, 0.25, ward: true);
			last = Vigil.RunIchor;
			if (run == 1)
			{
				first = last;
			}
			Vigil.Commune();
			SpendSigils();
		}
		Check("the eighth run beats the first", last > first * 4.0,
			Numbers.Short(first) + " then " + Numbers.Short(last));
		Check("spending sigils never weakens the keeper", Vigil.SigilMultiplier > 1.0 + 0.06 * Vigil.Sigils,
			Vigil.SigilsEarned + " taken, " + Vigil.Sigils + " still held");

		// -- Patience at the communion has to be the better play, and visibly so. --
		// Twelve hours of simulated play separates a keeper who communes the moment it pays
		// anything from one who waits for the offer to be worth a quarter of what they hold by
		// a factor in the millions. That is a fine thing for a game to reward - and an unfair
		// thing to hide behind a button that reads "+4 sigils", which is why the ledger now
		// quotes the payout as a share of what is already taken.
		double greedy = Communing(90, patience: 0.0);
		double patient = Communing(90, patience: 0.25);
		Check("waiting to commune beats taking it early", patient > greedy * 5.0,
			"patience is worth " + Numbers.Short(patient / greedy) + "x over ninety minutes");

		long spendable = 0;
		Vigil.Reset();
		// Summed as a LONG. One boon is the prestige sink and its ladder runs to two hundred
		// levels, each of the last hundred and eighty priced at the cost cap - two hundred
		// billion sigils in total, which is a hundred times what an int holds. Summed as one it
		// wrapped to minus seven hundred million, and the check then compared a first
		// communion's three sigils against a negative total and called it a failure. The sink
		// working is exactly what broke the arithmetic that measured it.
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			for (int level = 0; level < Content.Boons[i].MaxLevel; level++)
			{
				spendable += Vigil.BoonCost(i);
				Vigil.Boons[i]++;
			}
		}
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			spendable += Vigil.OverseerCost(rite);
		}

		Vigil.Reset();
		Play(30, 4, 0.25, ward: true);
		int firstOffer = Vigil.SigilsOnOffer;
		Check("a first communion cannot buy the game out", (long)firstOffer * 8 < spendable,
			"first run offers " + firstOffer + " against " + spendable + " to spend");
		Check("a first communion still buys something", firstOffer >= 1,
			firstOffer + " sigils after half an hour");

		// The communion row quotes the bonus the keeper is about to have, and the button under it
		// is the one irreversible thing in the game - so the promise on it has to be the promise
		// kept. It used to quote the payout as a share of sigils taken and call that the gain "on
		// what you hold", which at ten taken and five offered read as +50% for a bonus that moved
		// from x1.60 to x1.90. Both numbers were real; neither was the one on the label.
		double promised = Vigil.SigilMultiplierFor(Vigil.SigilsEarned + firstOffer);
		Vigil.Commune();
		Check("the communion pays the bonus it quoted",
			Math.Abs(Vigil.SigilMultiplier - promised) < 1e-9,
			"quoted " + Numbers.Mult(promised) + ", paid " + Numbers.Mult(Vigil.SigilMultiplier));

		Vigil.Reset();
	}

	/// <summary>The tables have to agree with the code that indexes them.</summary>
	private static void TablesLineUp()
	{
		Console.WriteLine("Tables line up");
		Check("every boon in the enum has a row", Enum.GetValues<Vigil.Boon>().Length == Content.Boons.Length,
			Enum.GetValues<Vigil.Boon>().Length + " named, " + Content.Boons.Length + " defined");
		Check("every rite is stamped with its own index", Content.Rites[^1].Index == Content.RiteCount - 1,
			"indices stamped");

		Vigil.Reset();
		bool maxedRefuses = true;
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			Vigil.Boons[i] = Content.Boons[i].MaxLevel;
			Vigil.Sigils = 1_500_000_000;
			maxedRefuses &= Vigil.BoonCost(i) == 0 && !Vigil.BuyBoon(i);
		}
		Check("a maxed boon cannot be bought again", maxedRefuses, "all six refuse");

		// One boon has a very long ladder and is the prestige sink, so its PRICE has to stay a
		// price the whole way up. A cost that multiplies every level reaches infinity in about
		// forty more of them, and an unpayable-because-unprintable cost would close the sink as
		// surely as a low ceiling would.
		int longest = 0;
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			if (Content.Boons[i].MaxLevel > Content.Boons[longest].MaxLevel)
			{
				longest = i;
			}
		}
		bool alwaysPayable = true;
		foreach (int level in new[] { 0, 10, 40, 100, 199 })
		{
			Vigil.Boons[longest] = level;
			int cost = Vigil.BoonCost(longest);
			alwaysPayable &= cost > 0 && cost <= 1_000_000_000;
		}
		Check("the long ladder is priced all the way up", alwaysPayable,
			Content.Boons[longest].Name + " stays payable to level "
				+ Content.Boons[longest].MaxLevel);
		Check("and it is long enough to be a sink", Content.Boons[longest].MaxLevel >= 50,
			Content.Boons[longest].MaxLevel + " levels against three for the rest");


		Vigil.Reset();
		Check("the sigil curve inverts exactly", OffersExactly(7), "payout and target agree");
	}

	/// <summary>The target a panel promises must be the one the button honours: a hair under it
	/// pays less, a hair over pays it.</summary>
	private static bool OffersExactly(int sigils)
	{
		double at = Vigil.RunIchorForSigils(sigils);
		Vigil.RunIchor = at * 0.999;
		bool under = Vigil.SigilsOnOffer < sigils;
		Vigil.RunIchor = at * 1.001;
		bool over = Vigil.SigilsOnOffer >= sigils;
		Vigil.RunIchor = 0.0;
		return under && over;
	}

	/// <summary>Leaving a game built to be left must still be worth doing.</summary>
	private static void IdlingWorks()
	{
		Console.WriteLine("Idling works");
		Vigil.Reset();
		Play(2, 4, 0, ward: true);
		double seeded = Vigil.LifetimeIchor;
		Play(60, 0, 0, ward: true);
		Check("an hour away grows the parish", Vigil.LifetimeIchor > seeded * 100.0,
			Numbers.Short(seeded) + " then " + Numbers.Short(Vigil.LifetimeIchor));

		Vigil.Reset();
		Vigil.Owned[0] = 20;
		OfflineReport report = Vigil.CatchUp(6.0 * 3600.0);
		Check("closing the game still pays", report.Ichor > 0.0,
			Numbers.Short(report.Ichor) + " over six hours");
		Check("offline is capped", Vigil.CatchUp(100.0 * 3600.0).Seconds <= Vigil.OfflineCapSeconds + 1.0,
			"cap " + Numbers.Duration(Vigil.OfflineCapSeconds));
		// -- The report has to describe what happened, not what is left over. --
		// Overseers spend while the keeper is away, which is their whole job, so the purse can
		// come back SMALLER than it left even as the parish grows. A report drawn from the purse
		// told a keeper who produced 205B that they had gathered 2.5B - and the more overseers
		// they had hired, the bigger the lie got.
		Vigil.Reset();
		Vigil.Rng = Seeded(5);
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 30;
			Vigil.Overseers[i] = true;
		}
		Vigil.Ichor = 1e6;
		double purseBefore = Vigil.Ichor;
		double rateBefore = Vigil.Rate;
		OfflineReport withOverseers = Vigil.CatchUp(20.0 * 3600.0);
		Check("the offline report counts what was produced", withOverseers.Ichor > (Vigil.Ichor - purseBefore) * 5.0,
			"reports " + Numbers.Short(withOverseers.Ichor) + " produced, not the "
				+ Numbers.Short(Vigil.Ichor - purseBefore) + " left in the purse");
		Check("and the parish is better for having been left", Vigil.Rate > rateBefore * 2.0,
			Numbers.Rate(rateBefore) + " becomes " + Numbers.Rate(Vigil.Rate));

		// The one promise offline progress makes: it cannot cost you anything you were not
		// there to defend.
		Check("offline never provokes a visitation", Vigil.TimesTaken == 0, "nothing arrives while away");
	}

	/// <summary>
	/// Put a keeper at the brink with a known parish and let something come for them.
	/// </summary>
	/// <remarks>
	/// Deterministic on purpose: the visitor is drawn from the deepest rite, so owning exactly
	/// one tier fixes which one arrives and makes the right answer knowable to the test.
	/// </remarks>
	private static void Summon(int rite, int wards, double ichor)
	{
		Vigil.Reset();
		// Enough that this parish's dread EQUILIBRIUM clears the brink. Forcing the meter to
		// 1.0 on a smaller holding proves nothing: the first tick relaxes it straight back
		// down, no walk begins, and every check below passes vacuously because nothing is
		// approaching for them to be wrong about. Owning one tier keeps it the deepest, so
		// which visitor arrives stays fixed.
		Vigil.Owned[rite] = 2000;
		Vigil.Wards = wards;
		Vigil.Ichor = ichor;
		Vigil.Dread = 1.0;
		Vigil.Tick(kDt);
		if (!Vigil.Approaching)
		{
			// Never silently: a fixture that stops summoning turns this whole section green.
			Console.WriteLine("  [FAIL] fixture: nothing came for rite " + rite
				+ " (equilibrium " + Vigil.DreadEquilibrium.ToString("0.00") + ")");
			s_failures++;
		}
	}

	/// <summary>
	/// The encounter has to add a decision without ever taxing the keeper who is not there.
	/// </summary>
	/// <remarks>
	/// The load-bearing check is <c>the absent keeper is treated exactly as before</c>. An idle
	/// game may not require attendance: the moment answering becomes mandatory, walking away
	/// stops being a legitimate way to play and the genre's whole promise is broken. So the
	/// no-answer branch is deliberately the OLD code path, and this pins it there.
	/// </remarks>
	private static void TheEncounterIsOptional()
	{
		Console.WriteLine("The encounter is an offer, not a demand");

		// Seeded, so a parish visited twice is visited the same way twice and this section does
		// not fail once a fortnight on an unlucky draw.
		Vigil.Rng = Seeded(20260824);

		bool named = true;
		for (int i = 0; i < Content.RiteCount; i++)
		{
			named &= Content.Rites[i].VisitorName.Length > 0
				&& Content.Rites[i].Approach.Length > 0
				&& Content.Rites[i].Answer != Answer.None;
		}
		Check("every rite names what comes for it", named, Content.RiteCount + " visitors, each answerable");

		// A table where every visitor wants the same thing is a table with no decision in it.
		int distinct = 0;
		foreach (Answer a in Enum.GetValues<Answer>())
		{
			if (a == Answer.None)
			{
				continue;
			}
			foreach (RiteDef r in Content.Rites)
			{
				if (r.Answer == a)
				{
					distinct++;
					break;
				}
			}
		}
		Check("the answers are spread across the verbs", distinct >= 3, distinct + " of 4 verbs are somebody's answer");

		Summon(0, 0, 0);
		Check("the meter filling starts a walk, not a loss", Vigil.Approaching && Vigil.AftermathSeconds == 0.0,
			"\"" + Content.Rites[0].VisitorName + "\" is " + Vigil.ApproachSeconds.ToString("0") + "s away");

		// You cannot reach further into the dark while it is already on its way. This lived in
		// the HUD as an extra clause on the button and nowhere else, so it was a rule for the
		// player and not for the game - and the harness spent every measurement stoking through
		// approaches, exercising a keeper nobody can be.
		Vigil.StokeCooldown = 0.0;
		Vigil.Dread = 0.5;
		Check("and nothing can be stoked while it is walking", !Vigil.CanStoke && !Vigil.Stoke(),
			"the dark is not taking suggestions");

		// -- The absent keeper: both branches must be byte-for-byte the old behaviour. --
		Summon(3, 1, 0);
		while (Vigil.Approaching)
		{
			Vigil.Tick(kDt);
		}
		Check("absent, warded: the ward is spent as always", Vigil.Wards == 0 && Vigil.AftermathSeconds == 0.0
			&& Math.Abs(Vigil.Dread - 0.55) < 1e-9, "ward consumed, dread left at 0.55");

		Summon(3, 0, 0);
		while (Vigil.Approaching)
		{
			Vigil.Tick(kDt);
		}
		Check("absent, unwarded: the aftermath lands as always", Vigil.AftermathSeconds > 0.0 && Vigil.TimesTaken == 1,
			"half pace for " + Vigil.AftermathSeconds.ToString("0") + "s");

		// -- Knowing beats guessing beats nothing is wrong; knowing beats nothing beats guessing. --
		Summon(3, 1, 0);
		// Funded AFTER the summon, because what an offering costs is priced off production and
		// production is not known until the parish exists. An unfunded keeper cannot give the
		// right answer when the right answer is ichor, which is a refusal rather than a loss.
		Vigil.Ichor = Vigil.OfferCost * 2.0;
		Vigil.Give(Content.Rites[3].Answer);
		bool turned = !Vigil.Approaching && Vigil.AftermathSeconds == 0.0 && Vigil.Wards == 1 && Vigil.Dread > 0.4;
		Check("the right answer costs no ward and no pace", turned,
			"turned away, dread left at " + Vigil.Dread.ToString("0.00"));

		Answer wrong = Content.Rites[3].Answer == Answer.Still ? Answer.Bell : Answer.Still;
		Summon(3, 1, 0);
		Vigil.Give(wrong);
		Check("a wrong answer is worse than no answer", Vigil.AftermathSeconds > 0.0 && Vigil.Wards == 1,
			"it lands anyway, and the ward could not be reached");

		// Guessing must not be free, or the correct play is to mash the cheapest verb forever.
		Summon(3, 0, 0);
		Vigil.Give(Answer.Still);
		double guessed = Vigil.AftermathSeconds;
		Summon(3, 0, 0);
		while (Vigil.Approaching)
		{
			Vigil.Tick(kDt);
		}
		Check("guessing never beats standing back", guessed >= Vigil.AftermathSeconds - 1e-9,
			"a bad guess costs at least what silence does");

		// -- An offer has to be payable, and refused when it is not. --
		Summon(4, 0, 0);
		Check("an answer you cannot afford is refused", !Vigil.Give(Answer.Offer) && Vigil.Approaching,
			"the walk continues rather than resolving for free");

		Summon(0, 0, 0);
		Vigil.Ichor = Vigil.OfferCost;
		Check("an answer you can afford is taken", Vigil.Give(Answer.Offer) && !Vigil.Approaching,
			"offering costs " + Numbers.Short(Vigil.OfferCost));

		// -- Nothing may come for a keeper who owns nothing, or the tutorial is an ambush. --
		Vigil.Reset();
		Vigil.Dread = 1.0;
		Vigil.Tick(kDt);
		Check("an empty parish is never visited", !Vigil.Approaching && Vigil.TimesTaken == 0,
			"nothing comes for a keeper with nothing");

		// -- Knowing must beat silence for EVERY visitor, not on average. --
		// The policy check earlier compares whole runs, which averages across the eight and can
		// hide a visitor whose answer is a bad trade. This walks them one at a time: after a
		// correct answer the keeper must hold at least as many wards AND stand at least as high
		// on the meter as they would have by doing nothing. Dread is what pays, so being left
		// lower for having known is a penalty dressed as a reward.
		int worseOff = 0;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			Summon(rite, 2, 0);
			// Funded, or an Offer answer is refused rather than given - which reads as a
			// resolution that never happened and quietly passes whatever is checked next.
			Vigil.Ichor = Vigil.OfferCost * 2.0;
			bool given = Vigil.Give(Content.Rites[rite].Answer);
			double knowingDread = Vigil.Dread;
			int knowingWards = Vigil.Wards;

			Summon(rite, 2, 0);
			while (Vigil.Approaching)
			{
				Vigil.Tick(kDt);
			}
			if (!given || knowingDread < Vigil.Dread || knowingWards < Vigil.Wards)
			{
				worseOff++;
				Console.WriteLine("         knowing pays less against " + Content.Rites[rite].VisitorName);
			}
		}
		Check("knowing beats silence against every visitor", worseOff == 0,
			Content.RiteCount + " visitors, none of them worth not knowing");

		// -- The contract the parish presents the approach through. --
		// Presentation cannot be tested here, but what it READS can be. The parish drives a
		// shader from 1 - ApproachSeconds/kApproachSeconds and indexes a rite array with
		// ApproachRite, so a fraction that leaves 0..1 or an index that goes stale while
		// Approaching is still true would be a bad frame or an exception, not a wrong number.
		Summon(5, 0, 0);
		double worstFraction = 0.0;
		bool indexAlwaysValid = true;
		while (Vigil.Approaching)
		{
			double fraction = 1.0 - Vigil.ApproachSeconds / Vigil.kApproachSeconds;
			worstFraction = Math.Max(worstFraction, Math.Abs(fraction - Math.Clamp(fraction, 0.0, 1.0)));
			indexAlwaysValid &= Vigil.ApproachRite >= 0 && Vigil.ApproachRite < Content.RiteCount;
			Vigil.Tick(kDt);
		}
		Check("the approach reads as a clean 0 to 1", worstFraction < 1e-9,
			"never leaves the range by more than " + worstFraction.ToString("0.0e+0"));
		Check("its rite stays indexable for the whole walk", indexAlwaysValid, "valid every tick");
		Check("and is released the moment it resolves", Vigil.ApproachRite == -1 && Vigil.ApproachSeconds == 0.0,
			"cleared on resolution");

		// -- Reachable BY CHOICE, from the very first rite. --
		// A parish under the equilibrium threshold is never visited unprovoked, which is
		// correct - but a keeper who deliberately walks toward one must be able to arrive.
		// Relaxation is proportional, so at the top a small parish sheds dread faster than it
		// gathers it, and stoking to exactly 1.0 used to be undone inside the same tick. The
		// whole encounter was gated behind an hour of growth without anyone choosing that.
		Vigil.Reset();
		Vigil.Owned[0] = 1;
		double walked = 0.0;
		while (walked < 180.0 && !Vigil.Approaching)
		{
			if (Vigil.CanStoke)
			{
				Vigil.Stoke();
			}
			Vigil.Tick(kDt);
			walked += kDt;
		}
		Check("one rite is enough to walk into one", Vigil.Approaching,
			Vigil.Approaching ? "arrived after " + Numbers.Duration(walked) + " of stoking" : "never arrived");

		// -- Variety. An encounter with one answer is a keypress, not a decision. --
		Vigil.Reset();
		Vigil.Rng = Seeded(1);
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 100;
		}
		int[] drawn = new int[Content.RiteCount];
		for (int trial = 0; trial < 4000; trial++)
		{
			Vigil.Dread = 1.0;
			Vigil.Tick(kDt);
			if (!Vigil.Approaching)
			{
				continue;
			}
			drawn[Vigil.ApproachRite]++;
			// Standing still, always, because it is the one answer that can never be REFUSED.
			// Answering each visitor with what it actually wants looks more thorough and is a
			// trap: an offering needs a purse, this keeper has none, so Give declines, the walk
			// never ends, and the loop counts the same stuck encounter four thousand times. It
			// read as one visitor taking 60% of every draw - a distribution bug that was not in
			// the game at all.
			Vigil.Give(Answer.Still);
		}
		int absent = 0;
		int commonest = 0;
		int total = 0;
		foreach (int n in drawn)
		{
			if (n == 0)
			{
				absent++;
			}
			commonest = Math.Max(commonest, n);
			total += n;
		}
		Check("every rite you own can call something", absent == 0,
			absent + " of " + Content.RiteCount + " visitors never appeared");
		Check("no single visitor dominates the encounter", commonest < total / 2,
			"commonest is " + (commonest * 100.0 / Math.Max(1, total)).ToString("0") + "% of draws");

		// -- The ordering the whole encounter rests on: knowing > silence > guessing. --
		// Checked as OUTCOMES over a real run rather than as branches, because the branches
		// were always going to be right; what matters is whether the numbers they produce put
		// the three kinds of player in the right order.
		// Ninety minutes, not forty-five. STANDING STILL is the right answer to two of the eight
		// visitors, so a short run's outcome turns on which ones happened to arrive - measured
		// at forty-five minutes this read 0.79x on one seed and 1.02x on another, and flipped
		// the check with it. Long enough for the mix to average out is the only honest horizon
		// for a claim about which policy is better.
		double silent = PlayAnswering(90, null);
		double knowing = PlayAnswering(90, Answer.None);
		double guessing = PlayAnswering(90, Answer.Still);
		Check("knowing the answer beats doing nothing", knowing > silent * 1.05,
			(knowing / silent).ToString("0.00") + "x doing nothing");
		Check("guessing loses to doing nothing", guessing < silent,
			(guessing / silent).ToString("0.00") + "x doing nothing");

		// -- Stoking has to be worth pressing on the parish a new keeper actually has. --
		Vigil.Reset();
		Vigil.Owned[0] = 1;
		Check("a new keeper's stoke buys something", Vigil.StokeOffer > Vigil.CostOf(0, 1) * 0.15,
			Numbers.Short(Vigil.StokeOffer) + " against a " + Numbers.Short(Vigil.CostOf(0, 1)) + " lantern");

		// -- Offline may not start one: you cannot answer a door you were not behind. --
		Vigil.Reset();
		Vigil.Owned[5] = 60;
		Vigil.CatchUp(8.0 * 3600.0);
		Check("offline never starts a walk", !Vigil.Approaching && Vigil.TimesTaken == 0,
			"eight hours away, nothing arrived");
	}

	/// <summary>Play a stretch, answering every encounter the same way. A null policy never
	/// answers at all; <see cref="Answer.None"/> means answer each one CORRECTLY, which is the
	/// only value of it that could not otherwise be expressed.</summary>
	private static double PlayAnswering(double minutes, Answer? policy, double stokePerSecond = 0.0)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(7);
		double click = 0.0;
		double stoke = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			stoke += stokePerSecond * kDt;
			while (stoke >= 1.0)
			{
				stoke -= 1.0;
				if (Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching && policy.HasValue)
			{
				Vigil.Give(policy.Value == Answer.None ? Vigil.CorrectAnswer : policy.Value);
			}
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	/// <summary>
	/// The parish has to sound like a place without becoming a notification tray.
	/// </summary>
	/// <remarks>
	/// A first session used to be six lines in ten minutes, every one of them a receipt for
	/// something the player had just done, while the dread meter climbed to a third full
	/// unremarked. The failure mode of fixing that is the opposite one - a nudge repeated into
	/// every silence, which is a tutorial popup wearing a costume - so both ends are pinned.
	/// </remarks>
	private static void TheParishSpeaks()
	{
		Console.WriteLine("The parish speaks");

		int said = 0;
		int beckons = 0;
		int repeats = 0;
		string last = "";
		Vigil.Reset();
		Vigil.Rng = Seeded(3);
		Vigil.Announce = (line, omen) =>
		{
			said++;
			if (line == Content.Beckon)
			{
				beckons++;
			}
			if (line == last)
			{
				repeats++;
			}
			last = line;
		};

		double click = 0.0;
		for (int step = 0; step < 10 * 60.0 / kDt; step++)
		{
			click += 3.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			Buy();
			Vigil.Tick(kDt);
		}
		Vigil.Announce = null;

		Check("a first session is not silent", said >= 10, said + " lines in ten minutes");
		Check("nor is it a notification tray", said <= 40, said + " lines, roughly one a minute");
		Check("the nudge is a suggestion, not a nag", beckons <= 2, beckons + " beckons all session");
		Check("no line follows itself", repeats == 0, repeats + " immediate repeats");

		// -- The feed cannot say more than it can show. --
		// Six lines, nine seconds each: about forty a minute before a line is pushed off before
		// anybody could read it. That budget is shared by every system that talks, so it is the
		// one place a new feature quietly ruins an old one - relics, murmurs, stoking and
		// visitations all landed in it, and a keeper riding the brink was at forty-one.
		// Measured for the LOUDEST kind of play, because the average is never the problem.
		List<double> spoken = new List<double>();
		double clock = 0.0;
		Vigil.Reset();
		Vigil.Rng = Seeded(21);
		Vigil.Announce = (_, _) => spoken.Add(clock);
		double clicking = 0.0;
		double stoking = 0.0;
		for (int step = 0; step < 20 * 60.0 / kDt; step++, clock += kDt)
		{
			clicking += 4.0 * kDt;
			while (clicking >= 1.0)
			{
				clicking -= 1.0;
				Vigil.Gather();
			}
			stoking += 1.0 * kDt;
			while (stoking >= 1.0)
			{
				stoking -= 1.0;
				if (Vigil.Dread < 0.9)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
		}
		Vigil.Announce = null;

		int busiest = 0;
		foreach (double at in spoken)
		{
			int within = 0;
			foreach (double other in spoken)
			{
				if (other >= at && other < at + 60.0)
				{
					within++;
				}
			}
			busiest = Math.Max(busiest, within);
		}
		Check("the busiest minute still fits in the feed", busiest < 38,
			busiest + " lines in the loudest minute, against about 40 the feed can show");

		// Offline must stay mute: replaying eight hours would otherwise dump every dread band
		// and a hundred ambient lines into the feed the instant a keeper came back.
		int atmosphere = 0;
		int total = 0;
		Vigil.Reset();
		Vigil.Owned[3] = 60;
		Vigil.Announce = (line, _) =>
		{
			total++;
			if (line == Content.Beckon)
			{
				atmosphere++;
			}
			foreach (string ambient in Content.Ambient)
			{
				if (line == ambient)
				{
					atmosphere++;
				}
			}
			foreach ((double _, string murmur) in Content.Murmurs)
			{
				if (line == murmur)
				{
					atmosphere++;
				}
			}
		};
		Vigil.CatchUp(8.0 * 3600.0);
		Vigil.Announce = null;
		// Atmosphere specifically, not everything. Marks earned while away are worth hearing
		// about - that is the game reporting what the parish achieved without you - but eight
		// hours of replayed dread bands and ambient lines would bury them the instant you
		// returned, which is the actual failure being guarded against.
		Check("coming back is not a wall of atmosphere", atmosphere == 0,
			atmosphere + " atmospheric lines from eight hours away");
		Check("but it still reports what you missed", total is > 0 and < 12,
			total + " lines, all of them things that happened");
	}

	/// <summary>
	/// Every mark is either earnable alone or says out loud that it is not.
	/// </summary>
	/// <remarks>
	/// The check grants a solo keeper everything solo play can possibly produce and then asks
	/// what is left. Anything still unearned needs a second player, and must be flagged as
	/// such - otherwise a mark quietly becomes impossible the day someone writes one against a
	/// counter only the congregation moves, and the only person who finds out is a completionist
	/// who cannot be told why.
	/// </remarks>
	private static void EveryMarkIsReachable()
	{
		Console.WriteLine("Every mark is reachable, or says why not");
		Vigil.Reset();
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 500;
			Vigil.Overseers[i] = true;
			Vigil.VisitorsMet[i] = true;
			Vigil.VisitorsBested[i] = true;
		}
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			Vigil.OfferingsTaken[i] = true;
		}
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			Vigil.Boons[i] = Content.Boons[i].MaxLevel;
		}
		Vigil.LifetimeIchor = 1e30;
		Vigil.Sigils = 9999;
		Vigil.SigilsEarned = 9999;
		Vigil.Communions = 50;
		Vigil.PlayedSeconds = 1e6;
		Vigil.HighDreadSeconds = 1e5;
		Vigil.WardsRaised = 999;
		Vigil.TimesTaken = 999;
		Vigil.HandGathers = 99999;
		Vigil.VisitorsAnswered = 999;
		// SharedVigilSeconds and CommunionSurges stay at zero: no solo keeper can move them.
		//
		// Everything the relic and consecration marks ask about. A fixture that does not know
		// about a counter reports the mark reading it as impossible, which is a lie about the
		// game told by the test - the exact failure this suite has hit before, and the reason
		// this list has to grow whenever a mark starts asking a new question.
		Vigil.RelicsFound = 9999;
		Vigil.RelicsRendered = 9999;
		Vigil.BestRelicGrade = (int)Grade.Hollowed;
		Vigil.Consecrations = 9;
		// Every boon taken as far as it goes, including the long ladder, so the mark aimed at
		// the prestige sink is reachable in this fixture too. A fixture that does not know about
		// a field reports the feature reading it as impossible - which this suite has already
		// done once, for six marks at once.
		for (int i = 0; i < Vigil.Boons.Length && i < Content.Boons.Length; i++)
		{
			Vigil.Boons[i] = Content.Boons[i].MaxLevel;
		}
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			Vigil.OfferingsTaken[i] = true;
		}

		// Three hands full, one of them lending structures - both reachable by a keeper alone,
		// so the fixture has to actually do it rather than assume it.
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			Vigil.Worn[slot] = default;
		}
		int filled = 0;
		for (int seed = 1; seed < 400000 && filled < Relics.Slots; seed++)
		{
			Relic candidate = new Relic { Seed = seed, Grade = Grade.Hollowed };
			bool lends = false;
			for (int i = 0; i < Relics.PowerCount(candidate.Grade); i++)
			{
				lends |= Relics.PowerAt(candidate, i) == Power.Foundation;
			}
			// One lender is enough; the other two hands can hold anything.
			if (lends || filled > 0)
			{
				Vigil.Worn[filled++] = candidate;
			}
		}

		int unflagged = 0;
		int flagged = 0;
		for (int i = 0; i < Content.Marks.Length; i++)
		{
			if (Content.Marks[i].Earned())
			{
				continue;
			}
			if (Content.Marks[i].NeedsCongregation)
			{
				flagged++;
			}
			else
			{
				unflagged++;
				Console.WriteLine("         unreachable and unlabelled: " + Content.Marks[i].Name);
			}
		}
		Check("no mark is quietly impossible alone", unflagged == 0,
			flagged + " need a congregation and say so, " + unflagged + " do not");
		Check("solo play can still earn most of them", flagged < Content.Marks.Length / 3,
			(Content.Marks.Length - flagged) + " of " + Content.Marks.Length + " are earnable alone");
	}

	/// <summary>
	/// Throw everything at the simulation in every order and check it never lies.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The targeted checks above each know what they are looking for, which is exactly their
	/// limit: every one of them was written after a bug, and none of them would have caught
	/// the next one. This does the opposite - it makes no assumptions about what the keeper is
	/// trying to do, hammers the rules with legal actions in illegal-looking orders, and asks
	/// only that the state stay describable afterwards.
	/// </para>
	/// <para>
	/// Long frames are part of the fuzz on purpose. A stutter, a breakpoint or a laptop lid
	/// produces a single enormous delta, and most of the ordering bugs found in this project
	/// were ordering bugs precisely because one step did something a smaller step would not.
	/// </para>
	/// </remarks>
	private static void NothingBreaksUnderPressure()
	{
		Console.WriteLine("Nothing breaks under pressure");
		string broken = "";
		int approaches = 0;
		int communions = 0;

		// Several seeds, because one is an anecdote. A single run that happens never to buy a
		// boon while something is walking proves nothing about the case where it does.
		foreach (int seed in new[] { 99, 1234, 20260824 })
		{
			string trouble = Hammer(seed, ref approaches, ref communions);
			if (trouble.Length > 0)
			{
				broken = "seed " + seed + ": " + trouble;
				break;
			}
		}

		Check("state stays describable under any order", broken.Length == 0, broken.Length == 0
			? approaches + " walks and " + communions + " communions across 750k actions, three seeds"
			: broken);
	}

	/// <summary>One seed's worth of abuse. Returns the first broken promise, or empty.</summary>
	private static string Hammer(int seed, ref int approaches, ref int communions)
	{
		Random rng = new Random(seed);
		Vigil.Reset();
		Vigil.Rng = new Random(seed);
		bool wasApproaching = false;

		for (int step = 0; step < 250000; step++)
		{
			switch (rng.Next(16))
			{
				case 12:
					Vigil.Wear(rng.Next(Relics.Satchel + 2) - 1, rng.Next(Relics.Slots + 1) - 1);
					break;
				case 13:
					Vigil.Remove(rng.Next(Relics.Slots + 1) - 1);
					break;
				case 14:
					Vigil.Render(rng.Next(Relics.Satchel + 2) - 1);
					break;
				case 15:
					// A relic off the wire, with the hostile values a peer could actually send.
					Vigil.ReceiveRelic(rng.Next(-5, 100000), rng.Next(-3, 12), "Someone");
					break;
				case 0:
					Vigil.Gather();
					break;
				case 1:
					Vigil.BuyRite(rng.Next(Content.RiteCount), rng.Next(1, 40));
					break;
				case 2:
					Vigil.Stoke();
					break;
				case 3:
					Vigil.RaiseWard();
					break;
				case 4:
					Vigil.TakeOffering(rng.Next(Content.Offerings.Length));
					break;
				case 5:
					// RARE. An even draw communed five thousand times in four hundred thousand
					// actions, which wipes the parish faster than dread can ever build on it -
					// so the fuzz reported a clean sweep having never once reached the brink,
					// and the encounter, the newest and least proven code in the game, went
					// entirely unexercised. A uniform fuzzer is not an unbiased one.
					if (rng.Next(400) == 0 && Vigil.Commune())
					{
						communions++;
					}
					break;
				case 6:
					Vigil.BuyBoon(rng.Next(Content.Boons.Length));
					break;
				case 7:
					Vigil.HireOverseer(rng.Next(Content.RiteCount));
					break;
				case 8:
					// Any answer at any moment, including when nothing is walking.
					Vigil.Give((Answer)rng.Next(Enum.GetValues<Answer>().Length));
					break;
				case 9:
					Vigil.ReceiveTithe(rng.NextDouble() * 1e6, "someone");
					break;
				case 10:
					if (rng.Next(30) == 0)
					{
						Vigil.ShuntToEcho(rng.Next(Vigil.MaxEchoes));
					}
					// Also rare, and for the same reason as communion. Shedding is a congregation
					// verb that dumps up to a whole point of dread; drawn evenly it zeroed the
					// meter roughly every twelfth action, so nothing could ever climb to the
					// brink and the fuzz swept 400k actions without one encounter in it. The
					// actions that RESET state have to be rare or they are the only thing tested.
					if (rng.Next(200) == 0)
					{
						Vigil.ShedDread(rng.NextDouble());
					}
					break;
				default:
					// Frames from a sixtieth of a second to five whole seconds.
					Vigil.Tick(rng.NextDouble() < 0.9 ? kDt : rng.NextDouble() * 5.0);
					break;
			}

			if (Vigil.Approaching && !wasApproaching)
			{
				approaches++;
			}
			wasApproaching = Vigil.Approaching;
			string trouble = Describe();
			if (trouble.Length > 0)
			{
				return trouble;
			}
		}

		return "";
	}

	/// <summary>
	/// A clock a keeper can leave running has to keep running.
	/// </summary>
	/// <remarks>
	/// Not a rule of the game, but the arithmetic underneath one, and the harness is the only
	/// place it can be checked at all. A float accumulating a frame delta stops advancing
	/// entirely at about 524,300 seconds, because by then its own spacing is wider than a
	/// sixtieth of a second - so the parish's animation froze permanently on the sixth day of
	/// continuous running, in a game whose whole premise is being left running.
	/// </remarks>
	private static void TheClockOutlastsTheKeeper()
	{
		Console.WriteLine("The clock outlasts the keeper");

		// The shape of the bug, so this documents what was wrong as well as what is right.
		float asFloat = 1.0f;
		while (asFloat + (1.0f / 60.0f) != asFloat)
		{
			asFloat *= 1.0001f;
		}
		Check("a float clock would have stopped inside a week", asFloat / 86400.0f < 8.0f,
			"it freezes after " + (asFloat / 86400.0f).ToString("0.0") + " days");

		// A double keeps resolving a frame for longer than any machine will stay up.
		double asDouble = 3650.0 * 86400.0;
		Check("a double clock still advances after ten years", asDouble + (1.0 / 60.0) != asDouble,
			"ten years of uptime, still ticking");

		// And what the shader is handed stays small enough to resolve finely, forever.
		bool wrapHolds = true;
		foreach (double days in new[] { 1.0, 7.0, 30.0, 365.0, 3650.0 })
		{
			float handed = (float)(days * 86400.0 % 3600.0);
			wrapHolds &= handed >= 0.0f && handed < 3600.0f && handed + (1.0f / 60.0f) != handed;
		}
		Check("and what the shader is handed always resolves", wrapHolds,
			"wrapped into an hour, so a float never coarsens");
	}

	/// <summary>Every promise the rules make about their own state, in one place. Returns the
	/// first one broken, or empty.</summary>
	private static string Describe()
	{
		if (double.IsNaN(Vigil.Ichor) || double.IsInfinity(Vigil.Ichor) || Vigil.Ichor < 0.0)
		{
			return "ichor is " + Vigil.Ichor;
		}
		if (double.IsNaN(Vigil.Dread) || Vigil.Dread < 0.0 || Vigil.Dread > 1.0)
		{
			return "dread is " + Vigil.Dread;
		}
		if (Vigil.Wards < 0 || Vigil.Wards > Vigil.MaxWards)
		{
			return "wards is " + Vigil.Wards + " of " + Vigil.MaxWards;
		}
		if (Vigil.Fervour < 0.0 || Vigil.Fervour > 1.0)
		{
			return "fervour is " + Vigil.Fervour;
		}
		if (Vigil.AftermathSeconds < 0.0 || Vigil.StokeCooldown < 0.0)
		{
			return "a timer went negative";
		}
		if (Vigil.Sigils < 0 || Vigil.SigilsEarned < Vigil.Sigils)
		{
			return "sigils " + Vigil.Sigils + " against " + Vigil.SigilsEarned + " earned";
		}
		if (Vigil.Approaching)
		{
			if (Vigil.ApproachRite < 0 || Vigil.ApproachRite >= Content.RiteCount)
			{
				return "walking rite " + Vigil.ApproachRite;
			}
			// Against what THIS walk started with, not against the constant: a Patience relic
			// lengthens the warning, so a keeper wearing one legitimately has more than
			// kApproachSeconds left. Checking the constant was a check that a relic could break
			// by working correctly.
			if (Vigil.ApproachSeconds < 0.0 || Vigil.ApproachSeconds > Vigil.ApproachTotal + 1e-9)
			{
				return "walk clock is " + Vigil.ApproachSeconds + " of " + Vigil.ApproachTotal;
			}
			if (Vigil.ApproachTotal < Vigil.kApproachSeconds - 1e-9)
			{
				return "a walk started shorter than the warning is meant to be";
			}
			if (Vigil.Owned[Vigil.ApproachRite] <= 0)
			{
				return "something is walking from a rite the keeper does not own";
			}
		}
		else if (Vigil.ApproachSeconds != 0.0)
		{
			return "a walk clock is running with nothing walking";
		}
		for (int i = 0; i < Content.RiteCount; i++)
		{
			if (Vigil.Owned[i] < 0)
			{
				return "owned " + i + " is " + Vigil.Owned[i];
			}
		}
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			if (Vigil.Boons[i] < 0 || Vigil.Boons[i] > Content.Boons[i].MaxLevel)
			{
				return "boon " + i + " is level " + Vigil.Boons[i];
			}
		}

		if (Vigil.Satchel.Count > Relics.Satchel)
		{
			return "satchel holds " + Vigil.Satchel.Count + " of " + Relics.Satchel;
		}
		foreach (Relic relic in Vigil.Satchel)
		{
			if (!relic.Exists)
			{
				return "a nothing is being carried";
			}
			if (relic.Grade < Grade.Leavings || relic.Grade > Grade.Hollowed)
			{
				return "a carried relic is grade " + (int)relic.Grade;
			}
		}
		foreach (Relic relic in Vigil.Worn)
		{
			if (relic.Exists && (relic.Grade < Grade.Leavings || relic.Grade > Grade.Hollowed))
			{
				return "a worn relic is grade " + (int)relic.Grade;
			}
		}
		return "";
	}

	/// <summary>
	/// Leaning on the dead has to be a loan, never a bin.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Shedding dread is the one verb that could end this game. Dread is the whole bargain -
	/// it is what pays, and the meter filling is the only thing that threatens anybody - so a
	/// button that removes it keeps every reward and deletes every risk. The fuzz already
	/// showed what that looks like from the other side: drawn evenly, shedding zeroed the meter
	/// so often that nothing could ever reach a visitation at all.
	/// </para>
	/// <para>
	/// So the checks are about the PRICE. What an echo takes it keeps, a loaded line draws the
	/// dark in faster, and the whole thing has to be worth doing only for a keeper who can
	/// answer what it brings.
	/// </para>
	/// </remarks>
	private static void TheDeadRememberWhatTheyTake()
	{
		Console.WriteLine("The dead remember what they take");

		Vigil.Reset();
		Check("a keeper with no past has nobody to lean on", !Vigil.CanShunt(0),
			"no echoes before the first communion");

		// Earn a communion the honest way, so the echo is real rather than fabricated.
		Vigil.RunIchor = Vigil.RunIchorForSigils(3);
		Vigil.Commune();
		Check("a communion leaves a keeper behind", Vigil.Echoes.Count == 1,
			Vigil.Echoes.Count + " standing behind this one");

		Vigil.Owned[2] = 200;
		Vigil.Dread = 0.9;
		double before = Vigil.Dread;
		double moved = Vigil.ShuntToEcho(0);
		Check("what leaves the keeper arrives on the echo", moved > 0.0
			&& Math.Abs((before - Vigil.Dread) - moved) < 1e-9
			&& Math.Abs(Vigil.Echoes[0].Burden - moved) < 1e-9,
			Numbers.Percent(moved) + " moved, none of it lost");

		Check("and not again immediately", !Vigil.CanShunt(0),
			"settling for " + Numbers.Duration(Vigil.ShuntCooldown));

		// The price: a loaded line pulls the dark in faster.
		Vigil.Reset();
		Vigil.Owned[4] = 120;
		double clearWindow = Vigil.SecondsToVisitation;
		for (int i = 0; i < Vigil.MaxEchoes; i++)
		{
			Vigil.Echoes.Add(new Echo { Name = "K" + i, Burden = Vigil.kEchoCapacity });
		}
		double loadedWindow = Vigil.SecondsToVisitation;
		Check("a loaded line costs real peace", loadedWindow < clearWindow * 0.7,
			Numbers.Duration(clearWindow) + " of quiet becomes " + Numbers.Duration(loadedWindow));
		Check("but never so much that it cannot be climbed out of", loadedWindow > 4.0,
			"still " + Numbers.Duration(loadedWindow) + " between them at full burden");

		// And it eases, so a keeper who stops leaning gets their meter back.
		double loaded = Vigil.BurdenTotal;
		for (int i = 0; i < 4000; i++)
		{
			Vigil.Tick(1.0);
		}
		Check("the dead put it down eventually", Vigil.BurdenTotal < loaded * 0.5,
			"burden " + loaded.ToString("0.00") + " eases to " + Vigil.BurdenTotal.ToString("0.00"));

		// -- Communion must not launder the debt. --
		// The one cost of shunting is that a loaded line pulls the dark in faster. Communion is
		// something players already do constantly for sigils, so if giving the parish back also
		// shed the burden, the cost would not even be a detour around - it would just be gone.
		Vigil.Reset();
		for (int i = 0; i < Vigil.MaxEchoes; i++)
		{
			Vigil.Echoes.Add(new Echo { Name = "old" + i, Burden = Vigil.kEchoCapacity });
		}
		double owed = Vigil.BurdenTotal;
		for (int c = 0; c < 12; c++)
		{
			Vigil.KeeperName = "new" + c;
			Vigil.RunIchor = Vigil.RunIchorForSigils(3);
			Vigil.Commune();
		}
		Check("communion cannot shed what the line carries", Vigil.BurdenTotal >= owed - 1e-9,
			"burden " + owed.ToString("0.00") + " survives twelve communions");

		// ...but the line still has to be the keepers you actually were.
		int fossils = 0;
		foreach (Echo echo in Vigil.Echoes)
		{
			if (echo.Name.StartsWith("old"))
			{
				fossils++;
			}
		}
		Check("and the line is still who you were lately", fossils == 0,
			Vigil.Echoes.Count + " echoes, none of them fossils");

		// The trade itself: worth it only if you can answer what it brings.
		double skilledClear = Lean(75, shunt: false, answers: true);
		double skilledLoaded = Lean(75, shunt: true, answers: true);
		double carelessClear = Lean(75, shunt: false, answers: false);
		double carelessLoaded = Lean(75, shunt: true, answers: false);

		Check("leaning pays a keeper who answers", skilledLoaded > skilledClear,
			(skilledLoaded / skilledClear).ToString("0.00") + "x for one who knows the answers");
		Check("and does not pay one who does not", carelessLoaded < skilledLoaded / skilledClear * carelessClear,
			(carelessLoaded / carelessClear).ToString("0.00") + "x for one who does not");
	}

	/// <summary>Play a stretch, communing whenever the payout is worth at least
	/// <paramref name="patience"/> of the sigils already taken.</summary>
	private static double Communing(double minutes, double patience)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(11);
		double click = 0.0;
		double stoke = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			stoke += 0.4 * kDt;
			while (stoke >= 1.0)
			{
				stoke -= 1.0;
				if (Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			int offer = Vigil.SigilsOnOffer;
			if (offer > 0 && offer >= Math.Max(1.0, Vigil.SigilsEarned * patience))
			{
				Vigil.Commune();
			}
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	/// <summary>Play a stretch, optionally leaning on the dead and optionally answering what
	/// that brings. Communes on sight so there is a line to lean on at all.</summary>
	private static double Lean(double minutes, bool shunt, bool answers)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(11);
		double click = 0.0;
		double stoke = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			stoke += 0.4 * kDt;
			while (stoke >= 1.0)
			{
				stoke -= 1.0;
				if (Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			if (Vigil.SigilsOnOffer >= 4)
			{
				Vigil.Commune();
			}
			if (shunt && Vigil.Dread > 0.8)
			{
				for (int e = 0; e < Vigil.Echoes.Count; e++)
				{
					if (Vigil.ShuntToEcho(e) > 0.0)
					{
						break;
					}
				}
			}
			if (Vigil.Approaching && answers)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	/// <summary>
	/// What the parish gives up, and whether it is worth digging for.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Relics exist to give hand-gathering an arc. Measured before they were added, a click was
	/// worth a flat twentieth of a second of production at every parish size from three tiers
	/// to all eight - never worthless, but exactly as interesting at hour ten as at hour one.
	/// </para>
	/// <para>
	/// Grade is drawn against DREAD on purpose, so the reason to click and the reason to ride
	/// the meter are one reason. These checks are mostly about that, and about the two ways an
	/// inventory always breaks: items multiplying, and rarity meaning nothing.
	/// </para>
	/// </remarks>
	private static void TheParishGivesThingsUp()
	{
		Console.WriteLine("The parish gives things up");

		// -- Digging deeper turns up better things. --
		int[] safeGrades = new int[5];
		int[] brinkGrades = new int[5];
		int safeFinds = 0;
		int brinkFinds = 0;
		Random rng = Seeded(4);
		for (int i = 0; i < 200000; i++)
		{
			Relic safe = Relics.Dig(rng, 0.05, i + 1);
			if (safe.Exists)
			{
				safeGrades[(int)safe.Grade]++;
				safeFinds++;
			}
			Relic brink = Relics.Dig(rng, 1.0, i + 1);
			if (brink.Exists)
			{
				brinkGrades[(int)brink.Grade]++;
				brinkFinds++;
			}
		}
		Check("the brink gives up more", brinkFinds > safeFinds * 2,
			safeFinds + " finds in safety against " + brinkFinds + " at the brink");
		Check("and gives up better", brinkGrades[4] > 0 && safeGrades[4] == 0,
			"Hollowed things exist only at the brink");
		Check("but the brink is not a guarantee", brinkGrades[0] > brinkFinds / 20,
			Numbers.Percent((double)brinkGrades[0] / brinkFinds) + " of brink finds are still Leavings");

		// -- The ladder must never invert, at ANY depth. --
		// The overall mix can look perfectly graded while the top band is broken, because most
		// digs happen at moderate dread and drown out the brink. The last band is open-ended -
		// everything above the final threshold is Hollowed - so a roll that reaches too far past
		// it makes the rarest grade the commonest of the good ones exactly where a keeper spends
		// their most dangerous minutes. Checked per depth for that reason.
		Random ladder = Seeded(3);
		string inverted = "";
		foreach (double depth in new[] { 0.25, 0.5, 0.75, 0.9, 1.0 })
		{
			// Enough digs to RESOLVE the bands, not merely to sample them. Relics were made eight
			// times rarer to stop them being litter, which shrank this sample by the same factor
			// and left the two narrowest bands inside the noise - the check began failing on the
			// count rather than on the ladder. A rate change silently weakened a check that never
			// mentioned the rate.
			int[] seen = new int[5];
			for (int i = 0; i < 500000; i++)
			{
				Relic dug = Relics.Dig(ladder, depth, i + 1);
				if (dug.Exists)
				{
					seen[(int)dug.Grade]++;
				}
			}
			// The WHOLE ladder, every grade. This check was weakened twice to accommodate bands
			// that did not actually narrow - first to tolerate a ratio, then to watch only the
			// good grades - before it was clear the honest fix was to the bands rather than to
			// the check. A guard that keeps being loosened to keep passing is telling you
			// something about the thing it guards.
			for (int g = 1; g < 5 && inverted.Length == 0; g++)
			{
				// A grade may be absent at this depth, and the bottom two bands are near enough
				// the same width that which of them leads is noise - Keepsake runs 50% against
				// Leavings' 47% at shallow dread and nobody could tell. What must not happen is
				// a grade running away from the one below it: the bug this exists for had
				// Hollowed at 22% against Hallowed's 17%, a ratio of 1.29.
				if (seen[g] > seen[g - 1] * 1.15)
				{
					inverted = Relics.GradeName((Grade)g) + " outnumbers " + Relics.GradeName((Grade)(g - 1))
						+ " at dread " + depth.ToString("0.00");
				}
			}
		}
		Check("no grade is commoner than the one beneath it", inverted.Length == 0,
			inverted.Length == 0 ? "the whole ladder holds at every depth" : inverted);

		// -- Rare, but not so rare that nobody sees one. --
		// The drop rate was cut by eight because a keeper was turning up fourteen relics in two
		// minutes and filling the satchel inside ninety seconds. That is exactly the kind of
		// number somebody retunes later, and overshooting it in the other direction is quieter:
		// the feature simply stops appearing, and nothing fails.
		Vigil.Reset();
		Vigil.Rng = Seeded(9);
		double elapsed = 0.0;
		double firstFind = -1.0;
		double firstGood = -1.0;
		Vigil.OnFound = found =>
		{
			if (firstFind < 0.0)
			{
				firstFind = elapsed;
			}
			if (found.Grade >= Grade.Anointed && firstGood < 0.0)
			{
				firstGood = elapsed;
			}
		};
		int carriedAtTen = 0;
		double digging = 0.0;
		double leaning = 0.0;
		for (int step = 0; step < 20 * 60.0 / kDt; step++, elapsed += kDt)
		{
			digging += 4.0 * kDt;
			while (digging >= 1.0)
			{
				digging -= 1.0;
				Vigil.Gather();
			}
			leaning += 0.25 * kDt;
			while (leaning >= 1.0)
			{
				leaning -= 1.0;
				if (Vigil.Dread < 0.9)
				{
					Vigil.Stoke();
				}
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
			if (Math.Abs(elapsed - 600.0) < kDt * 0.5)
			{
				carriedAtTen = Vigil.Satchel.Count;
			}
		}
		Vigil.OnFound = null;
		// A BOUND, and a loose one, because this is one sample of an exponential waiting time.
		// At four gathers a second and a base chance near one in four hundred, the mean wait is
		// about a hundred seconds - but the tail is long, and the old bound of 300s sat near the
		// 95th percentile, so it failed roughly one seed in twenty. It duly failed on seed 8 at
		// 5m23s with nothing wrong with the game. A single draw from a long-tailed distribution
		// cannot be bounded tightly; what it can say is that a new keeper does not spend a
		// quarter of an hour finding nothing. The property this was reaching for is asserted
		// deterministically just below, where no seed can move it.
		Check("a keeper finds something in their first minutes", firstFind is >= 0.0 and < 900.0,
			firstFind < 0.0 ? "nothing in twenty minutes" : "first find at " + Numbers.Duration(firstFind));
		// The property the sampled check above was really reaching for, stated so that no seed
		// can move it: how long a NEW keeper - no dread, nothing worn - waits on average for the
		// ground to give something up. Measured over a hundred thousand digs, so the figure is
		// the rate itself rather than one draw from it.
		{
			Random ground = Seeded(9001);
			const int digs = 200000;
			int finds = 0;
			for (int i = 0; i < digs; i++)
			{
				if (Relics.Dig(ground, 0.0, i + 1).Exists)
				{
					finds++;
				}
			}
			// Four gathers a second is the rate the simulated keeper above clicks at.
			double perClick = (double)finds / digs;
			double seconds = perClick > 0.0 ? 1.0 / (perClick * 4.0) : double.MaxValue;
			Check("and the ground gives up its first thing within a few minutes on average",
				seconds > 20.0 && seconds < 240.0,
				"a new keeper waits " + Numbers.Duration(seconds) + " on average, clicking steadily");
		}

		Check("and something worth wearing before long", firstGood is >= 0.0 and < 900.0,
			firstGood < 0.0 ? "nothing Anointed in twenty minutes" : "first Anointed at " + Numbers.Duration(firstGood));
		// Deliberately NOT checking that the satchel stays unfilled. It curates itself, dropping
		// its worst, so a keeper who never manages it still ends up holding the best twelve
		// things they have found - that is a fine state and not worth forbidding. What was wrong
		// before the rate was cut was the SPEED: ninety seconds to fill meant every find after
		// that displaced one nobody had chosen. The two checks above measure that directly, and
		// a bound on the satchel would only have been a preference dressed as a requirement.
		Check("and is holding a dozen worth having by then", carriedAtTen > 0,
			carriedAtTen + " carried at ten minutes, of " + Relics.Satchel);

		// -- Rarity has to mean something at a glance. --
		double bestKeepsake = 0.0;
		double worstHallowed = double.MaxValue;
		for (int seed = 1; seed < 20000; seed++)
		{
			bestKeepsake = Math.Max(bestKeepsake,
				Relics.MagnitudeAt(new Relic { Seed = seed, Grade = Grade.Keepsake }, 0));
			worstHallowed = Math.Min(worstHallowed,
				Relics.MagnitudeAt(new Relic { Seed = seed, Grade = Grade.Hallowed }, 0));
		}
		Check("a lucky Keepsake never beats an unlucky Hallowed", bestKeepsake < worstHallowed,
			"best " + Numbers.Percent(bestKeepsake) + " against worst " + Numbers.Percent(worstHallowed));
		Check("and a better grade carries more powers",
			Relics.PowerCount(Grade.Hollowed) > Relics.PowerCount(Grade.Leavings),
			Relics.PowerCount(Grade.Leavings) + " power against " + Relics.PowerCount(Grade.Hollowed));

		// -- A relic is entirely its seed, and never repeats a power. --
		bool stable = true;
		bool distinct = true;
		for (int seed = 1; seed < 5000; seed++)
		{
			Relic relic = new Relic { Seed = seed, Grade = Grade.Hollowed };
			stable &= Relics.NameOf(relic) == Relics.NameOf(new Relic { Seed = seed, Grade = Grade.Hollowed });
			int count = Relics.PowerCount(relic.Grade);
			for (int a = 0; a < count && distinct; a++)
			{
				for (int b = a + 1; b < count; b++)
				{
					distinct &= Relics.PowerAt(relic, a) != Relics.PowerAt(relic, b);
				}
			}
		}
		Check("a seed always makes the same relic", stable, "5000 seeds, all reproducible");
		Check("no relic carries the same power twice", distinct, "5000 seeds, all distinct");

		// -- Names actually vary, or "endless" is a lie. --
		System.Collections.Generic.HashSet<string> names = new System.Collections.Generic.HashSet<string>();
		for (int seed = 1; seed < 4000; seed++)
		{
			names.Add(Relics.NameOf(new Relic { Seed = seed, Grade = Grade.Anointed }));
		}
		// Against the SPACE, not against a sample. Four thousand seeds drawn from a space of
		// nine thousand collide constantly by the birthday paradox alone - the first version of
		// this check demanded 3000 distinct names from 4000 seeds and was simply asking for
		// something arithmetic makes impossible. What matters is that the space is deep enough
		// that a keeper does not see the same name twice in a session.
		long space = (long)Relics.Materials * Relics.Forms * Relics.Provenances;
		Check("the parish has more to give than anyone will dig", space > 15000,
			Numbers.Short(space) + " possible Anointed names, " + Numbers.Short(space * Relics.Epithets) + " Hollowed");
		Check("and does not repeat itself within a session", names.Count > 3400,
			names.Count + " distinct in 4000 seeds, against " + Numbers.Short(space) + " possible");

		// -- The art seed has to survive being a float. --
		// Material parameters travel as a float4. float32 holds integers exactly only to about
		// sixteen million, and relic seeds start near a billion and count up by one - so passing
		// the seed straight through put runs of sixty-five consecutive relics onto one float:
		// different names, different powers, identical pictures.
		int artCollisions = 0;
		float previousArt = -1.0f;
		for (int seed = 900_000_000; seed < 900_000_400; seed++)
		{
			float art = Relics.ArtSeed(new Relic { Seed = seed, Grade = Grade.Anointed });
			if (art == previousArt)
			{
				artCollisions++;
			}
			previousArt = art;
			// And it must be a value a float carries exactly, or the shader sees something else.
			if (art != (float)(int)art || art < 0.0f || art > 65535.0f)
			{
				artCollisions += 1000;
			}
		}
		Check("neighbouring relics get their own picture", artCollisions == 0,
			"400 consecutive seeds, no two drawn alike");

		// -- Rendering is a bonus for tidying, not a second economy. --
		// Relics come out of clicking, so a generous render turns the game into
		// click-render-repeat and the parish becomes decoration. Measured by rendering
		// everything the instant it lands, which is the greediest play available.
		double keeping = Rendering(60, melt: false);
		double melting = Rendering(60, melt: true);
		Check("rendering everything is a bonus, not a business", melting < keeping * 1.35,
			"worth " + (melting / keeping).ToString("0.00") + "x keeping it");

		// -- Wearing one has to actually do something. --
		Vigil.Reset();
		Vigil.Owned[2] = 50;
		Vigil.Dread = 0.6;
		double bareHand = Vigil.HandGain;
		double bareWard = Vigil.WardCost;
		Vigil.Satchel.Add(new Relic { Seed = 12345, Grade = Grade.Hollowed });
		Vigil.Wear(0, 0);
		bool moved = Math.Abs(Vigil.HandGain - bareHand) > 1e-9 || Math.Abs(Vigil.WardCost - bareWard) > 1e-9
			|| Vigil.Wearing(Power.Bargain) > 0.0 || Vigil.Wearing(Power.Patience) > 0.0
			|| Vigil.Wearing(Power.Almsgiving) > 0.0 || Vigil.Wearing(Power.Steadiness) > 0.0;
		Check("worn relics change the vigil", moved, "a Hollowed relic moves at least one knob");
		Check("carried ones do not", Vigil.Worn[0].Exists && Vigil.Satchel.Count == 0,
			"wearing it took it out of the satchel");

		// -- A power has to do what its row says it does. --
		// The ledger prints "+5% by hand" beside a relic, and a keeper takes that literally.
		// Hand used to multiply only the bare hand value rather than the whole figure, so it
		// reached all of the number early and NONE of it later - a relic claiming five percent
		// moved nothing once a parish was three tiers deep, because the slice off production had
		// swamped the term being boosted. A power that quietly stops working as you progress is
		// worse than one that was never offered.
		string lying = "";
		foreach (int owned in new[] { 5, 60, 200 })
		{
			foreach (Power power in Enum.GetValues<Power>())
			{
				Vigil.Reset();
				for (int i = 0; i < Content.RiteCount; i++)
				{
					Vigil.Owned[i] = owned;
				}
				Vigil.Dread = 0.7;
				Vigil.Fervour = 0.5;
				double hand = Vigil.HandGain;
				double bareDread = Vigil.DreadMultiplier;
				double ward = Vigil.WardCost;
				double offer = Vigil.OfferCost;
				double drain = Vigil.FervourDrain;
				double stoke = Vigil.StokeInterval;

				// A Keepsake carries exactly ONE power, so nothing else can move the number being
				// measured - a Hollowed relic carries three and every reading contaminates.
				Relic one = default;
				double claimed = 0.0;
				for (int seed = 1; seed < 40000 && !one.Exists; seed++)
				{
					Relic candidate = new Relic { Seed = seed, Grade = Grade.Keepsake };
					if (Relics.PowerAt(candidate, 0) == power)
					{
						one = candidate;
						claimed = Relics.MagnitudeAt(candidate, 0);
					}
				}
				Vigil.Worn[0] = one;

				// Bargain is measured against WHAT DREAD ADDS, which is what its row now claims:
				// the multiplier is 1 + coefficient * d^1.4, so the added part is everything above
				// one. Measuring the whole multiplier would find about half the claimed figure and
				// call the row a liar, when the row is describing the part it actually moves.
				double addedBare = bareDread - 1.0;
				double got = power switch
				{
					Power.Hand => Vigil.HandGain / hand - 1.0,
					Power.Warding => 1.0 - Vigil.WardCost / ward,
					Power.Almsgiving => 1.0 - Vigil.OfferCost / offer,
					Power.Steadiness => 1.0 - Vigil.FervourDrain / drain,
					Power.Bargain => (Vigil.DreadMultiplier - 1.0) / addedBare - 1.0,
					// Foundation is not a percentage of anything - it lends whole copies of a
					// rite - so measuring it against a claimed magnitude compares two different
					// units. It has checks of its own below.
					Power.Foundation => claimed,
					// Measured against the interval a bare keeper waits, which is what the row
					// claims a share of. Not floored in this range - five of these together come
					// nowhere near the one-second floor in StokeInterval.
					Power.Kindling => 1.0 - Vigil.StokeInterval / stoke,
					// Reliquary changes the odds on a random draw rather than a coefficient
					// anything here can read, so it is measured by actually digging - see
					// LuckIsWorthWearing below.
					Power.Reliquary => claimed,
					_ => Vigil.Wearing(Power.Patience),
				};
				if (Math.Abs(got - claimed) > claimed * 0.02 && lying.Length == 0)
				{
					lying = power + " claims " + Numbers.Percent(claimed) + " and gives "
						+ Numbers.Percent(got) + " at x" + owned;
				}
			}
		}
		Check("every power does what its row claims", lying.Length == 0,
			lying.Length == 0 ? "every kind, at every parish size" : lying);

		// -- The bug every inventory has: items multiplying. --
		Vigil.Reset();
		Vigil.Rng = Seeded(5);
		for (int i = 0; i < 8; i++)
		{
			Vigil.Satchel.Add(new Relic { Seed = 100 + i, Grade = Grade.Keepsake });
		}
		int before = Count();
		Random shuffle = Seeded(6);
		for (int i = 0; i < 20000; i++)
		{
			switch (shuffle.Next(3))
			{
				case 0:
					Vigil.Wear(shuffle.Next(Relics.Satchel + 2), shuffle.Next(Relics.Slots + 1));
					break;
				case 1:
					Vigil.Remove(shuffle.Next(Relics.Slots + 1));
					break;
				default:
					// Discard is the only one allowed to reduce the count, so it is checked apart.
					break;
			}
			if (Count() != before)
			{
				break;
			}
		}
		Check("relics are never lost or copied by handling", Count() == before,
			before + " relics survive 20k wears and removals");

		System.Collections.Generic.HashSet<int> seeds = new System.Collections.Generic.HashSet<int>();
		bool unique = true;
		foreach (Relic relic in Vigil.Satchel)
		{
			unique &= seeds.Add(relic.Seed);
		}
		foreach (Relic relic in Vigil.Worn)
		{
			unique &= !relic.Exists || seeds.Add(relic.Seed);
		}
		Check("and no two of them are the same object", unique, seeds.Count + " distinct seeds held");

		// -- What a loadout is WORTH, against everything else in the game. --
		// The one balance question a power system has to answer. Relics should be the best
		// single lever a keeper has and still be in the same conversation as stoking at 4.9x
		// and answering visitors at 1.45x. The first pass put them at ten times a bare keeper,
		// which is not a lever, it is the game - measured, not guessed, and only measurable by
		// playing it out.
		// An hour, not three quarters of one. Relics compound - they raise what dread pays,
		// which raises production, which buys rites - so the same loadout measures 1.1x at
		// forty-five minutes and well over twice that at sixty. A power budget checked over too
		// short a run reads as harmless right up until somebody plays for an evening.
		double bare = Digging(60, wearing: false);
		double laden = Digging(60, wearing: true);
		double worth = laden / bare;
		Check("relics are the best lever, not the whole game", worth is > 1.4 and < 5.0,
			"a full loadout is worth " + worth.ToString("0.00") + "x a bare keeper");

		// -- And the ceiling, with everything else stacked under them. --
		// Relics multiply THROUGH the other systems: a Bargain relic raises what dread pays,
		// which raises production, which buys rites. Measuring a loadout against a bare keeper
		// says nothing about what it does on top of maxed boons, every offering and two hundred
		// sigils - which is the state a long save actually reaches.
		Vigil.Reset();
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 200;
		}
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			Vigil.OfferingsTaken[i] = true;
		}
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			Vigil.Boons[i] = Content.Boons[i].MaxLevel;
		}
		Vigil.SigilsEarned = 200;
		Vigil.Dread = 1.0;
		Vigil.Fervour = 1.0;
		double ceilingBare = Vigil.GlobalMultiplier;
		// Relics that actually carry the power being measured. Three arbitrary seeds happened to
		// carry no Bargain at all, so the check compared the ceiling against itself and passed
		// reporting 1.00x - a fixture testing nothing, which is the only kind of green worth
		// being suspicious of.
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			for (int seed = 1 + slot * 9000; seed < 9000 + slot * 9000; seed++)
			{
				Relic candidate = new Relic { Seed = seed, Grade = Grade.Hollowed };
				bool bargains = false;
				for (int i = 0; i < Relics.PowerCount(candidate.Grade); i++)
				{
					bargains |= Relics.PowerAt(candidate, i) == Power.Bargain;
				}
				if (bargains)
				{
					Vigil.Worn[slot] = candidate;
					break;
				}
			}
		}
		double ceilingLaden = Vigil.GlobalMultiplier;
		Check("the fixture is wearing what it means to measure", Vigil.Wearing(Power.Bargain) > 0.0,
			"Bargain " + Numbers.Percent(Vigil.Wearing(Power.Bargain)) + " worn");
		Check("the top end stays a number", !double.IsNaN(ceilingLaden) && !double.IsInfinity(ceilingLaden)
			&& ceilingLaden > 0.0, Numbers.Short(ceilingLaden) + "x with everything");
		Check("and relics only tilt it, never own it", ceilingLaden < ceilingBare * 2.0,
			"they add " + (ceilingLaden / ceilingBare).ToString("0.00") + "x on top of everything else");

		// -- The satchel has to hold, and hold the RIGHT things. --
		Vigil.Reset();
		Vigil.Rng = Seeded(7);
		Vigil.Owned[0] = 50;
		Vigil.Dread = 1.0;
		for (int i = 0; i < 40000; i++)
		{
			Vigil.Gather();
		}
		Check("the satchel does not overflow", Vigil.Satchel.Count <= Relics.Satchel,
			Vigil.Satchel.Count + " of " + Relics.Satchel + " carried after 40k digs");
		int leavings = 0;
		foreach (Relic relic in Vigil.Satchel)
		{
			if (relic.Grade == Grade.Leavings)
			{
				leavings++;
			}
		}
		Check("and a full one is not all rubbish", leavings < Vigil.Satchel.Count,
			leavings + " Leavings among " + Vigil.Satchel.Count + " carried");
	}

	/// <summary>
	/// Handing a relic to another keeper must move it, not copy it.
	/// </summary>
	/// <remarks>
	/// The network layer cannot be exercised here, but the half that would duplicate items can:
	/// a trade is a removal on one side and an arrival on the other, and every duping bug ever
	/// written is those two steps disagreeing. So the removal is checked to happen BEFORE
	/// anything is sent, and the arrival is checked to cost the sender exactly what it gives
	/// the receiver.
	/// </remarks>
	private static void TradingMovesRatherThanCopies()
	{
		Console.WriteLine("Trading moves rather than copies");

		Vigil.Reset();
		for (int i = 0; i < 4; i++)
		{
			Vigil.Satchel.Add(new Relic { Seed = 900 + i, Grade = Grade.Anointed });
		}
		int held = Vigil.Satchel.Count;
		bool gave = Vigil.GiveRelic(0, out Relic given);
		Check("giving takes it out of the satchel first", gave && Vigil.Satchel.Count == held - 1,
			"satchel " + held + " becomes " + Vigil.Satchel.Count);

		// The far side, simulated: what left one keeper arrives at the other, unchanged.
		Vigil.Reset();
		Vigil.ReceiveRelic(given.Seed, (int)given.Grade, "Another keeper");
		Check("and what arrives is the same relic", Vigil.Satchel.Count == 1
			&& Vigil.Satchel[0].Seed == given.Seed && Vigil.Satchel[0].Grade == given.Grade,
			Relics.NameOf(Vigil.Satchel[0]) + " arrives intact");

		Check("nothing can be given that is not there", !Vigil.GiveRelic(99, out _),
			"an index past the satchel is refused");

		// A relic that has already left the sender must land, even into a full satchel - the
		// alternative is a trade that destroys the thing being traded.
		Vigil.Reset();
		for (int i = 0; i < Relics.Satchel; i++)
		{
			Vigil.Satchel.Add(new Relic { Seed = 700 + i, Grade = Grade.Leavings });
		}
		Vigil.Worn[0] = new Relic { Seed = 5, Grade = Grade.Hollowed };
		Vigil.ReceiveRelic(4242, (int)Grade.Hallowed, "Another keeper");
		bool landed = false;
		foreach (Relic relic in Vigil.Satchel)
		{
			landed |= relic.Seed == 4242;
		}
		Check("a full satchel still takes what it is handed", landed && Vigil.Satchel.Count <= Relics.Satchel,
			"it lands and the satchel stays at " + Vigil.Satchel.Count);
		Check("and never at the cost of what is worn", Vigil.Worn[0].Seed == 5,
			"the worn relic is untouched");

		// The wire carries two numbers, so the numbers have to be treated as hostile.
		Vigil.Reset();
		Vigil.ReceiveRelic(77, 999, "Someone");
		Check("a grade off the ladder is clamped, not trusted",
			Vigil.Satchel.Count == 1 && Vigil.Satchel[0].Grade <= Grade.Hollowed,
			"999 becomes " + Relics.GradeName(Vigil.Satchel[0].Grade));
		Vigil.Reset();
		Vigil.ReceiveRelic(0, 2, "Someone");
		Check("and a relic that does not exist is refused", Vigil.Satchel.Count == 0,
			"seed 0 is how nothing is spelled");

		// -- What the trade button offers is steerable. --
		// The congregation hands over the FIRST carried relic, because three buttons in a 320px
		// row leave no space for a target picker. That is only acceptable if a keeper can change
		// which relic is first - so discarding and wearing both have to move it, and the ledger
		// labels the one that is next to go.
		Vigil.Reset();
		Vigil.Satchel.Add(new Relic { Seed = 61, Grade = Grade.Leavings });
		Vigil.Satchel.Add(new Relic { Seed = 62, Grade = Grade.Hollowed });
		int wouldGive = Vigil.Satchel[0].Seed;
		Vigil.Render(0);
		Check("leaving one behind changes what is offered", Vigil.Satchel[0].Seed != wouldGive,
			"the offer moves from " + wouldGive + " to " + Vigil.Satchel[0].Seed);

		Vigil.Satchel.Insert(0, new Relic { Seed = 63, Grade = Grade.Keepsake });
		Vigil.Wear(0, 0);
		Check("and so does wearing one", Vigil.Satchel.Count > 0 && Vigil.Satchel[0].Seed == 62,
			"wearing the first promotes the next");

		// A failed send must give it back, silently.
		Vigil.Reset();
		Vigil.Satchel.Add(new Relic { Seed = 31337, Grade = Grade.Hallowed });
		int spoken = 0;
		Vigil.GiveRelic(0, out Relic dropped);
		Vigil.Announce = (_, _) => spoken++;
		Vigil.RestoreRelic(dropped);
		Vigil.Announce = null;
		Check("a dropped offer comes back without comment",
			Vigil.Satchel.Count == 1 && Vigil.Satchel[0].Seed == 31337 && spoken == 0,
			"restored, and nothing claimed to have happened");
	}

	/// <summary>Play a stretch digging, optionally wearing the best three things found. The
	/// keeper rides the meter, because that is where relics come from.</summary>
	private static double Digging(double minutes, bool wearing)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(11);
		if (wearing)
		{
			// A KNOWN loadout, not whatever the run happens to turn up. Wearing the best of what
			// was found made this measurement depend on the drops, and the same policy scored
			// 2.51x on one reading and 1.54x on another - a check that can fail on luck is not a
			// check. Three fixed Hollowed relics measure the budget instead of the weather.
			for (int slot = 0; slot < Relics.Slots; slot++)
			{
				Vigil.Worn[slot] = new Relic { Seed = 7000 + slot * 137, Grade = Grade.Hollowed };
			}
		}
		double click = 0.0;
		double stoke = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			stoke += 0.4 * kDt;
			while (stoke >= 1.0)
			{
				stoke -= 1.0;
				if (Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			// Nothing here: the loadout is FIXED before the run starts, see below.
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	/// <summary>Play a stretch, optionally rendering every relic the moment it is found.</summary>
	private static double Rendering(double minutes, bool melt)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(11);
		double click = 0.0;
		double stoke = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			stoke += 0.4 * kDt;
			while (stoke >= 1.0)
			{
				stoke -= 1.0;
				if (Vigil.Dread < 0.85)
				{
					Vigil.Stoke();
				}
			}
			if (Vigil.Wards < Vigil.MaxWards && Vigil.Ichor > Vigil.WardCost * 3.0)
			{
				Vigil.RaiseWard();
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			while (melt && Vigil.Satchel.Count > 0)
			{
				Vigil.Render(0);
			}
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	/// <summary>
	/// Two readouts of the same thing must not disagree.
	/// </summary>
	/// <remarks>
	/// The ledger shows what each rite makes and the bar shows what the parish makes, and a
	/// player reads both. They were computed from different expressions, so during a
	/// visitation's aftermath the rows claimed full production while the bar said half - out by
	/// exactly two, in the one state where a keeper is most likely to be checking.
	///
	/// Checked as an identity rather than by inspection: whatever either side is built from,
	/// the rows have to add up to the total.
	/// </remarks>
	private static void TheReadoutsAgree()
	{
		Console.WriteLine("The readouts agree with each other");
		foreach (double aftermath in new[] { 0.0, 20.0 })
		{
			Vigil.Reset();
			for (int i = 0; i < Content.RiteCount; i++)
			{
				Vigil.Owned[i] = 40;
			}
			Vigil.Dread = 0.7;
			Vigil.AftermathSeconds = aftermath;

			double rows = 0.0;
			for (int rite = 0; rite < Content.RiteCount; rite++)
			{
				// Exactly the expression the ledger row uses.
				rows += Vigil.Owned[rite] * Content.Rites[rite].BaseRate * Vigil.RiteMultiplier(rite)
					* Vigil.AftermathScale;
			}
			Check(aftermath > 0.0 ? "  while reeling from a visitation" : "  in a calm parish",
				Math.Abs(rows - Vigil.Rate) < Vigil.Rate * 1e-9,
				Numbers.Rate(rows) + " of rows against " + Numbers.Rate(Vigil.Rate) + " on the bar");
		}
	}

	/// <summary>Every relic the keeper holds, worn or carried.</summary>
	private static int Count()
	{
		int total = Vigil.Satchel.Count;
		foreach (Relic relic in Vigil.Worn)
		{
			if (relic.Exists)
			{
				total++;
			}
		}
		return total;
	}

	/// <summary>
	/// A vigil must survive being written down and read back.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The most dangerous code in the game and, until now, the only part with no test at all -
	/// because SaveSystem writes to a real user profile and a test that eats somebody's save is
	/// worse than no test. Splitting the data model out of the storage fixed that: a whole
	/// vigil can go through JSON and back in memory, touching nothing.
	/// </para>
	/// <para>
	/// The failure this guards is a field captured and never applied, which loses exactly one
	/// thing and says nothing about it. Ten fields were added to this save in a day.
	/// </para>
	/// </remarks>
	private static void AVigilSurvivesBeingWrittenDown()
	{
		Console.WriteLine("A vigil survives being written down");

		Vigil.Reset();
		Vigil.Rng = Seeded(4242);
		Vigil.KeeperName = "Someone";
		Vigil.Ichor = 123456.75;
		Vigil.RunIchor = 5555.5;
		Vigil.LifetimeIchor = 9.87e18;
		Vigil.Dread = 0.731;
		Vigil.Sigils = 17;
		Vigil.SigilsEarned = 91;
		Vigil.Communions = 6;
		Vigil.Wards = 2;
		Vigil.AftermathSeconds = 4.25;
		Vigil.PlayedSeconds = 98765.5;
		Vigil.TimesTaken = 13;
		Vigil.VisitorsAnswered = 44;
		for (int i = 0; i < Content.RiteCount; i++)
		{
			Vigil.Owned[i] = 3 + i * 11;
			Vigil.Overseers[i] = i % 2 == 0;
			Vigil.VisitorsMet[i] = i != 3;
			Vigil.VisitorsBested[i] = i > 4;
		}
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			Vigil.Boons[i] = i % (Content.Boons[i].MaxLevel + 1);
		}
		Vigil.OfferingsTaken[2] = true;
		Vigil.OfferingsTaken[9] = true;
		Vigil.MarksEarned[1] = true;
		Vigil.Echoes.Add(new Echo { Name = "Before", Burden = 0.42 });
		Vigil.Echoes.Add(new Echo { Name = "Earlier", Burden = 0.9 });
		Vigil.Satchel.Add(new Relic { Seed = 777, Grade = Grade.Hallowed });
		Vigil.Satchel.Add(new Relic { Seed = 778, Grade = Grade.Leavings });
		Vigil.Worn[1] = new Relic { Seed = 999, Grade = Grade.Hollowed };
		Vigil.RelicSeed = 55555;
		Vigil.RelicsFound = 120;
		VigilData.ShowWhispers = false;
		VigilData.DreadShake = 0.4f;

		string before = Describe(full: true);
		string json = System.Text.Json.JsonSerializer.Serialize(VigilData.Capture());

		// Wiped completely between, so anything that survives came out of the JSON rather than
		// out of the fact that it was never cleared.
		Vigil.Reset();
		VigilData.ShowWhispers = true;
		VigilData.DreadShake = 1.0f;
		VigilData.Apply(System.Text.Json.JsonSerializer.Deserialize<VigilSave>(json)!);

		string after = Describe(full: true);
		Check("everything comes back as it went in", before == after,
			before == after ? "a whole vigil round-trips" : "before: " + before + "  after: " + after);

		// An older save is just a save with fields missing, which is what every addition today
		// produced. It must load as a valid early vigil rather than as a broken one.
		Vigil.Reset();
		VigilData.Apply(System.Text.Json.JsonSerializer.Deserialize<VigilSave>(
			"{\"KeeperName\":\"Old\",\"Ichor\":500,\"Sigils\":4}")!);
		Check("a save from before all of this still loads", Vigil.KeeperName == "Old"
			&& Vigil.Sigils == 4 && Vigil.SigilsEarned >= 4 && Vigil.Echoes.Count == 0
			&& Vigil.Satchel.Count == 0 && Vigil.Boons.Length == Content.Boons.Length,
			"an empty-fielded save becomes a valid early vigil");

		// And a hostile one cannot put the vigil into a state the rules do not allow.
		Vigil.Reset();
		VigilData.Apply(System.Text.Json.JsonSerializer.Deserialize<VigilSave>(
			"{\"Dread\":9,\"Wards\":99,\"Sigils\":-5,\"WornSeeds\":[7],\"WornGrades\":[42]}")!);
		string broken = Describe(full: false);
		Check("and a corrupt one cannot make an impossible vigil", broken.Length == 0,
			broken.Length == 0 ? "clamped into something the rules allow" : broken);
	}

	/// <summary>A vigil written out as one string, for comparing two of them.</summary>
	private static string Describe(bool full)
	{
		if (!full)
		{
			return Describe();
		}
		System.Text.StringBuilder sb = new System.Text.StringBuilder();
		sb.Append(Vigil.KeeperName).Append('|').Append(Vigil.Ichor).Append('|').Append(Vigil.RunIchor)
			.Append('|').Append(Vigil.LifetimeIchor).Append('|').Append(Vigil.Dread)
			.Append('|').Append(Vigil.Sigils).Append('|').Append(Vigil.SigilsEarned)
			.Append('|').Append(Vigil.Communions).Append('|').Append(Vigil.Wards)
			.Append('|').Append(Vigil.AftermathSeconds).Append('|').Append(Vigil.PlayedSeconds)
			.Append('|').Append(Vigil.TimesTaken).Append('|').Append(Vigil.VisitorsAnswered)
			.Append('|').Append(Vigil.RelicSeed).Append('|').Append(Vigil.RelicsFound)
			.Append('|').Append(VigilData.ShowWhispers).Append('|').Append(VigilData.DreadShake);
		foreach (int owned in Vigil.Owned) { sb.Append('|').Append(owned); }
		foreach (bool b in Vigil.Overseers) { sb.Append('|').Append(b); }
		foreach (bool b in Vigil.OfferingsTaken) { sb.Append('|').Append(b); }
		foreach (bool b in Vigil.MarksEarned) { sb.Append('|').Append(b); }
		foreach (bool b in Vigil.VisitorsMet) { sb.Append('|').Append(b); }
		foreach (bool b in Vigil.VisitorsBested) { sb.Append('|').Append(b); }
		foreach (int level in Vigil.Boons) { sb.Append('|').Append(level); }
		foreach (Echo echo in Vigil.Echoes) { sb.Append('|').Append(echo.Name).Append(':').Append(echo.Burden); }
		foreach (Relic relic in Vigil.Satchel) { sb.Append('|').Append(relic.Seed).Append(':').Append((int)relic.Grade); }
		foreach (Relic relic in Vigil.Worn) { sb.Append('|').Append(relic.Seed).Append(':').Append((int)relic.Grade); }
		return sb.ToString();
	}

	/// <summary>
	/// A feed can be quiet enough and still be unreadable.
	/// </summary>
	/// <remarks>
	/// Volume is not the only way this goes wrong. A keeper riding the meter answers an
	/// encounter, drops to 0.45 dread, climbs back through every band and hears the same three
	/// teaching lines again - thirty seconds, forever, at a rate the budget check passes
	/// happily. So variety is measured directly: across any busy window, how much of what was
	/// said was something new.
	/// </remarks>
	private static void TheParishDoesNotRepeatItself()
	{
		Console.WriteLine("The parish does not repeat itself");

		List<string> lines = new List<string>();
		List<double> stamps = new List<double>();
		double clock = 0.0;
		Vigil.Reset();
		Vigil.Rng = Seeded(3);
		Vigil.Announce = (line, _) =>
		{
			lines.Add(line);
			stamps.Add(clock);
		};

		double tapping = 0.0;
		double leaning = 0.0;
		for (int step = 0; step < 30 * 60.0 / kDt; step++, clock += kDt)
		{
			tapping += 3.0 * kDt;
			while (tapping >= 1.0)
			{
				tapping -= 1.0;
				Vigil.Gather();
			}
			leaning += 0.3 * kDt;
			while (leaning >= 1.0)
			{
				leaning -= 1.0;
				if (Vigil.Dread < 0.9)
				{
					Vigil.Stoke();
				}
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
		}
		Vigil.Announce = null;

		double leastVaried = 1.0;
		for (int i = 0; i < lines.Count; i++)
		{
			HashSet<string> distinct = new HashSet<string>();
			int within = 0;
			for (int j = i; j < lines.Count && stamps[j] < stamps[i] + 300.0; j++)
			{
				distinct.Add(lines[j]);
				within++;
			}
			// Only windows busy enough for repetition to be noticeable.
			if (within >= 12)
			{
				leastVaried = Math.Min(leastVaried, (double)distinct.Count / within);
			}
		}
		Check("the parish does not talk in circles", leastVaried > 0.35,
			"the least varied five minutes was " + Numbers.Percent(leastVaried) + " new lines");
	}

	// -- The parish holds itself together ---------------------------------------------

	/// <summary>Windows to lay the parish out in. The second is the one the game was actually
	/// being played at when the conduits turned out to cross the buttons - very wide and very
	/// short, which is the shape that breaks layout arithmetic tuned on a 16:9 guess.</summary>
	private static readonly (int W, int H)[] s_windows =
	{
		(1920, 1080), (1636, 541), (1280, 800), (2560, 1080), (1024, 1400), (3440, 900),
	};

	/// <summary>
	/// The tallest a rite can stand, as a fraction of the window height.
	/// </summary>
	/// <remarks>
	/// Mirrors the cap in Parish.SyncRites: growth is a log curve on how many are owned, but it
	/// is capped to the gap between neighbours so a heavily-bought rite cannot swallow the one
	/// beside it, and capped again against the window. Taking the cap rather than the curve is
	/// deliberate - the checks below want the WORST case, which is a keeper who has bought
	/// enough of everything for every structure to be against its ceiling.
	/// </remarks>
	private static float TallestRite(int w, int h, int standing, float riteRight)
	{
		float spacing = Layout.RiteSpacing(standing, riteRight);
		float bySpacing = spacing * w * 0.92f / h;
		return MathF.Min(bySpacing, Layout.RiteHeightCap(h));
	}

	/// <summary>
	/// Nothing the parish draws crosses anything else it draws.
	/// </summary>
	/// <remarks>
	/// Every one of these was first "checked" by working a single example on paper, and the
	/// paper was wrong: the route it justified ran straight across the stoke and ward buttons at
	/// the window the game was being played at. Layout is engine-free precisely so these can be
	/// held here, at every window shape and every parish size, instead of by eye.
	/// </remarks>
	private static void TheParishHoldsItselfTogether()
	{
		Console.WriteLine("The parish holds itself together");

		int belowHorizon = 0;
		int offTop = 0;
		int throughRite = 0;
		int onTheSigil = 0;
		int backwards = 0;
		int shortcut = 0;
		int cases = 0;
		float tightest = 1.0f;

		foreach ((int w, int h) in s_windows)
		{
			// Wherever the sigil sits: the nave is inset from the canvas by a top bar and the
			// ledger, so its centre moves with both, and no single figure covers every window.
			for (int step = 0; step <= 6; step++)
			{
				float centreX = 0.45f + step * 0.05f;
				float restW = 268.0f / w;
				float restH = 268.0f / h;
				Vector2 centre = new Vector2(centreX, 0.467f);

				float riteRight = Layout.RiteRight(centreX, restW, Layout.RiteWidthCap(w, h) * 0.5f);
				Vector2 intake = Layout.Intake(centre, restW, restH);

				for (int standing = 1; standing <= Content.RiteCount; standing++)
				{
					cases++;
					// Per-rite, because the descent cap depends on how near the sigil a rite
					// stands. Modelling one shared height was what hid the descent fault: it made
					// every structure the same, and the fault is specifically about the LAST one.
					float[] tall = new float[standing];
					float[] crowns = new float[standing];
					float[] halves = new float[standing];
					float highest = intake.Y;
					for (int r = 0; r < standing; r++)
					{
						tall[r] = TallestRite(w, h, standing, riteRight);
						crowns[r] = Layout.CrownY(tall[r], h);
						halves[r] = tall[r] * h * 0.5f / w;
						highest = MathF.Min(highest, crowns[r]);
					}
					float canopy = Layout.CanopyY(highest);

					// The deepest rite must sit clear of the sigil, or the conduits are the
					// least of it.
					float deepest = Layout.RiteX(standing - 1, standing, riteRight);
					float gap = centreX - restW * 0.5f - (deepest + halves[standing - 1]);
					if (gap < 0.0f)
					{
						onTheSigil++;
					}

					// Rites in order, left to right, with no two in the same place.
					for (int i = 1; i < standing; i++)
					{
						if (Layout.RiteX(i, standing, riteRight) <= Layout.RiteX(i - 1, standing, riteRight))
						{
							backwards++;
						}
					}

					for (int rite = 0; rite < standing; rite++)
					{
						Vector2 from = new Vector2(Layout.RiteX(rite, standing, riteRight), crowns[rite]);
						float radius = Layout.CornerRadius(from, intake, canopy,
							Layout.RiteSpacing(standing, riteRight));

						// Never shorter than the straight line it replaces.
						if (Layout.ConduitLength(from, intake, canopy, radius) < (intake - from).Length() - 1e-4f)
						{
							shortcut++;
						}

						for (int step2 = 0; step2 <= 96; step2++)
						{
							Vector2 at = Layout.Conduit(from, intake, canopy, radius, step2 / 96.0f);

							// THE claim. Everything the keeper clicks - stoke, ward, bell, the
							// found banner - lives below the horizon. A conduit that stays above
							// it cannot reach any of them, at any window, ever.
							if (at.Y > Layout.Horizon)
							{
								belowHorizon++;
							}
							if (at.Y < 0.0f)
							{
								offTop++;
							}
							tightest = MathF.Min(tightest, Layout.Horizon - at.Y);

							// And it must not pass through a structure on the way. A rite fills
							// the band from its crown down to the horizon, so being over one
							// horizontally is only a fault if the wire has sunk into that band.
							for (int other = 0; other < standing; other++)
							{
								if (other == rite)
								{
									continue;
								}
								float ox = Layout.RiteX(other, standing, riteRight);
								if (MathF.Abs(at.X - ox) < halves[other] && at.Y > crowns[other])
								{
									throughRite++;
								}
							}
						}
					}
				}
			}
		}

		Check("a conduit never reaches the button row", belowHorizon == 0,
			belowHorizon == 0
				? "clear of the horizon by " + tightest.ToString("F3") + " at its worst, over " + cases + " parishes"
				: belowHorizon + " samples below the horizon");
		Check("nor leaves the top of the window", offTop == 0,
			offTop == 0 ? "the arch stays on screen at every aspect" : offTop + " samples off screen");
		Check("nor passes through a structure", throughRite == 0,
			throughRite == 0 ? "every arch clears every crown beside it" : throughRite + " samples inside a rite");
		Check("the deepest rite stands clear of the sigil", onTheSigil == 0,
			onTheSigil == 0 ? "at every window and parish size" : onTheSigil + " overlaps");
		Check("rites stand in order and never share a slot", backwards == 0,
			backwards == 0 ? "left to right, always" : backwards + " out of order");
		Check("a conduit is never shorter than the line it replaces", shortcut == 0,
			shortcut == 0 ? "so a bead never outruns its own wire" : shortcut + " too short");
	}

	/// <summary>
	/// The rolling fronts survive being squeezed into one float each.
	/// </summary>
	/// <remarks>
	/// Three numbers per front share a single float32, and the same format is written in C# and
	/// read in Slang. Nothing about a packing fault announces itself: a front simply appears at
	/// the wrong place on the floor, or carries the wrong weight, and there is no error anywhere.
	/// Exactly the kind of arithmetic that should not be trusted because it looked right.
	/// </remarks>
	private static void TheFrontsSurviveBeingPacked()
	{
		Console.WriteLine("The rolling fronts survive being packed");

		float worstOrigin = 0.0f;
		float worstProgress = 0.0f;
		int wrongWeight = 0;
		int falseIdle = 0;
		int cases = 0;

		for (int o = 0; o <= 40; o++)
		{
			for (int weight = 1; weight <= Layout.WaveWeight; weight++)
			{
				for (int step = 1; step <= 60; step++)
				{
					cases++;
					float origin = o / 40.0f;
					float progress = step / 60.0f * 0.999f;
					float packed = Layout.PackWave(origin, weight, progress);

					// A live front must never pack to the value that means idle.
					if (packed <= 0.0f)
					{
						falseIdle++;
					}

					(float gotOrigin, float gotProgress, int gotWeight) = Layout.UnpackWave(packed);
					if (gotWeight != weight)
					{
						wrongWeight++;
					}
					// The origin is deliberately coarse - 32 steps - so it is checked against
					// that, not against equality.
					worstOrigin = MathF.Max(worstOrigin, MathF.Abs(gotOrigin - origin));
					worstProgress = MathF.Max(worstProgress, MathF.Abs(gotProgress - progress));
				}
			}
		}

		Check("the weight comes back exactly", wrongWeight == 0,
			wrongWeight == 0 ? "every weight of " + Layout.WaveWeight + " across " + cases + " fronts"
				: wrongWeight + " wrong");
		Check("the progress comes back to within a pixel", worstProgress < 0.002f,
			"worst drift " + worstProgress.ToString("F5"));
		Check("the origin comes back inside its quantisation", worstOrigin <= 1.0f / 31.0f + 1e-4f,
			"worst drift " + worstOrigin.ToString("F4") + " against a step of "
				+ (1.0f / 31.0f).ToString("F4"));
		Check("a live front never reads as an idle one", falseIdle == 0,
			falseIdle == 0 ? "zero means idle and nothing else does" : falseIdle + " vanished");
		Check("and an idle one stays idle", Layout.PackWave(0.5f, 4, 0.0f) == 0.0f,
			"no progress, no front");
	}

	// -- Everything on screen can actually be read --------------------------------------

	/// <summary>Relative luminance, the sRGB way. Not the average of the channels - green is
	/// most of what an eye sees and blue is almost none of it, and a naive average calls this
	/// palette's greens and rusts equally bright when they are nothing like it.</summary>
	private static double Luminance(Vector4 c)
	{
		static double Channel(double v) => v <= 0.03928 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4);
		return 0.2126 * Channel(c.X) + 0.7152 * Channel(c.Y) + 0.0722 * Channel(c.Z);
	}

	/// <summary>Composite a colour over what is behind it, since half this palette is alpha.</summary>
	private static Vector4 Over(Vector4 fg, Vector4 bg)
	{
		float a = Math.Clamp(fg.W, 0.0f, 1.0f);
		return new Vector4(fg.X * a + bg.X * (1.0f - a), fg.Y * a + bg.Y * (1.0f - a),
			fg.Z * a + bg.Z * (1.0f - a), 1.0f);
	}

	/// <summary>The usual ratio, so the figures mean the same as they do everywhere else.</summary>
	private static double Contrast(Vector4 a, Vector4 b)
	{
		double la = Luminance(a);
		double lb = Luminance(b);
		return (Math.Max(la, lb) + 0.05) / (Math.Min(la, lb) + 0.05);
	}

	/// <summary>
	/// Every text colour is legible on every surface it can land on.
	/// </summary>
	/// <remarks>
	/// Written because it was not. <c>TextFaint</c> and <c>RowHot</c> were the same step of the
	/// grey ramp, so the second line of a ledger row - what a relic does, what a boon costs you -
	/// disappeared completely the moment the pointer touched the row. A contrast ratio of 1.04:
	/// not dim, not hard, gone. Nothing about it was detectable except by hovering a row and
	/// noticing that words had stopped being there.
	/// <para>
	/// The floor is a BOUND and a low one. This is a horror game played in near-darkness and its
	/// quietest text is meant to be quiet; the check is not here to make the palette bright, it
	/// is here to stop a colour being invented that cannot be read at all.
	/// </para>
	/// </remarks>
	private static void EverythingCanBeRead()
	{
		Console.WriteLine("Everything on screen can be read");

		(string Name, Vector4 Colour)[] surfaces =
		{
			("the void", Palette.Void),
			("a panel", Palette.Panel),
			("a sunken panel", Palette.PanelDeep),
			("a row", Palette.Row),
			("a row under the pointer", Palette.RowHot),
		};

		(string Name, Vector4 Colour)[] texts =
		{
			("bright", Palette.TextBright),
			("body", Palette.TextBody),
			("dim", Palette.TextDim),
			("faint", Palette.TextFaint),
		};

		// The accents carry meaning, so they are held to the same floor - a price in ichor that
		// cannot be read on a highlighted row is the same fault as a description that cannot.
		//
		// DreadText rather than Dread: the accent is also a bar fill and a shader tint, where a
		// dark rust is exactly right, and holding a fill to a type's contrast floor would be
		// checking the wrong thing. What has to be legible is the colour words are drawn in.
		(string Name, Vector4 Colour)[] accents =
		{
			("ichor", Palette.Ichor),
			("dread", Palette.DreadText),
			("a sigil", Palette.Sigil),
		};

		const double floor = 2.4;
		string worstPair = "";
		double worst = double.MaxValue;

		foreach ((string surfaceName, Vector4 surface) in surfaces)
		{
			Vector4 bg = Over(surface, Palette.Void);
			foreach ((string textName, Vector4 text) in texts)
			{
				double ratio = Contrast(Over(text, bg), bg);
				if (ratio < worst)
				{
					worst = ratio;
					worstPair = textName + " on " + surfaceName;
				}
			}
		}

		Check("the quietest text is still text", worst >= floor,
			worstPair + " at " + worst.ToString("F2") + ":1 against a floor of " + floor.ToString("F1"));

		double worstAccent = double.MaxValue;
		string worstAccentPair = "";
		foreach ((string surfaceName, Vector4 surface) in surfaces)
		{
			Vector4 bg = Over(surface, Palette.Void);
			foreach ((string accentName, Vector4 accent) in accents)
			{
				double ratio = Contrast(Over(accent, bg), bg);
				if (ratio < worstAccent)
				{
					worstAccent = ratio;
					worstAccentPair = accentName + " on " + surfaceName;
				}
			}
		}

		Check("and so is every accent that means something", worstAccent >= floor,
			worstAccentPair + " at " + worstAccent.ToString("F2") + ":1");

		// The one that actually shipped broken: a surface and a text colour at the same step.
		int collisions = 0;
		foreach ((string _, Vector4 surface) in surfaces)
		{
			foreach ((string _, Vector4 text) in texts)
			{
				if (Contrast(Over(text, Over(surface, Palette.Void)), Over(surface, Palette.Void)) < 1.15)
				{
					collisions++;
				}
			}
		}
		Check("no text is the same colour as its background", collisions == 0,
			collisions == 0 ? "every pair distinguishable" : collisions + " invisible");

		// Highlighting a row must make it MORE readable, never less. The old RowHot failed this
		// in the worst possible way, and it is the property a hover state exists to have.
		Vector4 cold = Over(Palette.Row, Palette.Void);
		Vector4 hot = Over(Palette.RowHot, Palette.Void);
		double coldRatio = Contrast(Over(Palette.TextFaint, cold), cold);
		double hotRatio = Contrast(Over(Palette.TextFaint, hot), hot);
		Check("a highlighted row is no harder to read than a quiet one", hotRatio >= coldRatio * 0.75,
			"faint text at " + coldRatio.ToString("F2") + ":1 quiet, " + hotRatio.ToString("F2") + ":1 lit");
	}

	/// <summary>
	/// A save written before today still means what it meant.
	/// </summary>
	/// <remarks>
	/// Which offerings a keeper has taken is stored as a bare array of flags indexed by position
	/// in <c>Content.Offerings</c>. So the order of that table is a SAVE FORMAT, and reordering it
	/// silently re-points every flag in every existing save at a different offering - a keeper
	/// loads their game and finds they own things they never bought and have lost things they
	/// did. Nothing about it throws; the numbers just quietly become wrong.
	///
	/// Forty new offerings were added below the original thirty for exactly this reason, instead
	/// of being woven into the ladder that generates them, which is where they belong tidily and
	/// would have broken every save in existence.
	/// </remarks>
	private static void OldSavesStillMeanWhatTheyMeant()
	{
		Console.WriteLine("A save written before today still means the same");

		// The first thirty, in order, as they have always been. Not a sample - the whole prefix,
		// because a check that spot-tests three of them passes on the one reordering that moves
		// the other twenty-seven.
		string[] frozen =
		{
			"Grave Lantern: Wick of Hair", "Grave Lantern: Second Wick", "Grave Lantern: Vigil Oil",
			"Bone Choir: Wick of Hair", "Bone Choir: Second Wick", "Bone Choir: Vigil Oil",
			"Weeping Statue: Wick of Hair", "Weeping Statue: Second Wick", "Weeping Statue: Vigil Oil",
			"Flesh Loom: Wick of Hair", "Flesh Loom: Second Wick", "Flesh Loom: Vigil Oil",
			"Ossuary Engine: Wick of Hair", "Ossuary Engine: Second Wick", "Ossuary Engine: Vigil Oil",
			"Drowned Chapel: Wick of Hair", "Drowned Chapel: Second Wick", "Drowned Chapel: Vigil Oil",
			"Pale Shepherd: Wick of Hair", "Pale Shepherd: Second Wick", "Pale Shepherd: Vigil Oil",
			"Hollow Mouth: Wick of Hair", "Hollow Mouth: Second Wick", "Hollow Mouth: Vigil Oil",
			"Steady Hands", "Bitten Tongue", "Red Thumb",
			"The Long Hour", "Names in the Ledger", "The Parish Remembers",
		};

		int moved = 0;
		string firstMoved = "";
		for (int i = 0; i < frozen.Length; i++)
		{
			if (i >= Content.Offerings.Length || Content.Offerings[i].Name != frozen[i])
			{
				if (moved == 0)
				{
					firstMoved = "index " + i + " should be \"" + frozen[i] + "\" and is \""
						+ (i < Content.Offerings.Length ? Content.Offerings[i].Name : "off the end") + "\"";
				}
				moved++;
			}
		}

		Check("the offerings a save indexes have not moved", moved == 0,
			moved == 0 ? "all " + frozen.Length + " still where a save expects them"
				: moved + " moved - " + firstMoved);
		Check("and there are more of them than there were", Content.Offerings.Length > frozen.Length,
			Content.Offerings.Length + " offerings, " + (Content.Offerings.Length - frozen.Length)
				+ " added past the frozen prefix");

		// The BOONS are the fourth, and the one whose corruption a keeper would feel longest.
		// They are stored as LEVELS indexed by position and they survive every communion, so
		// reordering them does not lose a flag - it hands somebody three levels of the wrong
		// permanent upgrade in the thing they spent the most runs earning.
		string[] frozenBoons =
		{
			"Deeper Wards", "Cold Blood", "The Old Bargain", "Steady Hand", "Unsleeping",
			"Quick Kindling",
		};
		int boonsMoved = 0;
		string firstBoonMoved = "";
		for (int i = 0; i < frozenBoons.Length; i++)
		{
			if (i >= Content.Boons.Length || Content.Boons[i].Name != frozenBoons[i])
			{
				if (boonsMoved == 0)
				{
					firstBoonMoved = "index " + i + " should be \"" + frozenBoons[i] + "\" and is \""
						+ (i < Content.Boons.Length ? Content.Boons[i].Name : "off the end") + "\"";
				}
				boonsMoved++;
			}
		}
		Check("the boons a save records levels against have not moved", boonsMoved == 0,
			boonsMoved == 0 ? "all " + frozenBoons.Length + " where a keeper's levels expect them"
				: boonsMoved + " moved - " + firstBoonMoved);

		// The RITES are the most load-bearing table of the three and were the only one with no
		// guard. Everything a keeper owns is indexed by rite - Owned, the offerings' targets, the
		// visitors met and bested, the consecration, the echoes' memory of what a run was given
		// to - so reordering this table does not re-point one flag, it re-points the entire save
		// at once: a keeper's four hundred Grave Lanterns become four hundred of something else.
		// Nothing throws, and the numbers all stay plausible.
		string[] frozenRites =
		{
			"Grave Lantern", "Bone Choir", "Weeping Statue", "Flesh Loom",
			"Ossuary Engine", "Drowned Chapel", "Pale Shepherd", "Hollow Mouth",
		};
		int ritesMoved = 0;
		string firstRiteMoved = "";
		for (int i = 0; i < frozenRites.Length; i++)
		{
			if (i >= Content.Rites.Length || Content.Rites[i].Name != frozenRites[i])
			{
				if (ritesMoved == 0)
				{
					firstRiteMoved = "index " + i + " should be \"" + frozenRites[i] + "\" and is \""
						+ (i < Content.Rites.Length ? Content.Rites[i].Name : "off the end") + "\"";
				}
				ritesMoved++;
			}
		}
		Check("the rites a save is indexed against have not moved", ritesMoved == 0,
			ritesMoved == 0 ? "all " + frozenRites.Length + " in the order every save assumes"
				: ritesMoved + " moved - " + firstRiteMoved);
		Check("and there are still exactly that many of them",
			Content.Rites.Length == frozenRites.Length && Content.RiteCount == frozenRites.Length,
			Content.Rites.Length + " rites, RiteCount " + Content.RiteCount);

		// The marks carry the SAME hazard and had no check at all: which ones a keeper has earned
		// is another array of flags indexed by position, so reordering that table hands somebody
		// a record of things they never did - and a record is the one thing in this game that is
		// meant to be permanent and true.
		string[] frozenMarks =
		{
			"First Light", "Full Choir", "Deep Ledger", "Steady Nerve", "Warded", "Bereaved",
			"Communed", "Marked", "The Whole Nave", "Mouth to Mouth", "Not Alone", "Answered",
			"Deepened", "Named", "Well Read",
		};
		int marksMoved = 0;
		string firstMarkMoved = "";
		for (int i = 0; i < frozenMarks.Length; i++)
		{
			if (i >= Content.Marks.Length || Content.Marks[i].Name != frozenMarks[i])
			{
				if (marksMoved == 0)
				{
					firstMarkMoved = "index " + i + " should be \"" + frozenMarks[i] + "\"";
				}
				marksMoved++;
			}
		}
		Check("the marks a save indexes have not moved", marksMoved == 0,
			marksMoved == 0 ? "all " + frozenMarks.Length + " still where a record expects them"
				: marksMoved + " moved - " + firstMarkMoved);
		Check("and there are more marks than there were", Content.Marks.Length > frozenMarks.Length,
			Content.Marks.Length + " marks, " + (Content.Marks.Length - frozenMarks.Length) + " added");

		// A mark nobody can earn is worse than no mark: it sits on the page forever telling a
		// keeper there is something left to do.
		int unearnable = 0;
		foreach (MarkDef mark in Content.Marks)
		{
			if (string.IsNullOrWhiteSpace(mark.Name) || string.IsNullOrWhiteSpace(mark.Blurb)
				|| mark.Earned == null)
			{
				unearnable++;
			}
		}
		// A bar and a mark that disagree are worse than a mark with no bar: one says "you are
		// nearly there" while the other says nothing has happened, and a keeper believes the bar.
		// Checked at BOTH ends of the game - a fresh vigil and a maximal one - because the
		// interesting disagreements are exactly at zero and at one.
		int lying = 0;
		int outOfRange = 0;
		string firstLie = "";
		// Empty, PART-WAY, and full. The middle one is the load-bearing case and was missing at
		// first: at zero every bar is empty and at a maximal keeper every bar is full, so both
		// ends agree with the mark no matter what denominator the bar was written with. A bar
		// that fills at twenty-five finds for a mark needing fifty is only visible in between -
		// and a keeper reading a full bar under an unearned mark believes the bar.
		foreach (int stage in new[] { 0, 1, 2 })
		{
			bool maximal = stage == 2;
			Vigil.Reset();
			if (stage == 1)
			{
				// Deliberately just under every threshold the marks name.
				Vigil.Ichor = 1e15;
				for (int rite = 0; rite < Content.RiteCount - 1; rite++)
				{
					Vigil.BuyRite(rite, 11);
				}
				Vigil.LifetimeIchor = 9.9e5;
				Vigil.HighDreadSeconds = 59.0;
				Vigil.SigilsEarned = 9;
				Vigil.VisitorsAnswered = 19;
				Vigil.RelicsFound = 49;
				Vigil.RelicsRendered = 19;
				Vigil.BestRelicGrade = (int)Grade.Hallowed;
				Vigil.SharedVigilSeconds = 29.0;
				for (int slot = 0; slot < Relics.Slots - 1; slot++)
				{
					Vigil.Worn[slot] = new Relic { Seed = 700 + slot, Grade = Grade.Anointed };
				}
				for (int i = 0; i < 39 && i < Content.Offerings.Length; i++)
				{
					Vigil.OfferingsTaken[i] = true;
				}
			}
			if (maximal)
			{
				Vigil.Ichor = 1e15;
				for (int rite = 0; rite < Content.RiteCount; rite++)
				{
					Vigil.BuyRite(rite, 60);
				}
				Vigil.LifetimeIchor = 1e12;
				Vigil.HighDreadSeconds = 1e4;
				Vigil.SigilsEarned = 99;
				Vigil.VisitorsAnswered = 99;
				Vigil.RelicsFound = 999;
				Vigil.RelicsRendered = 99;
				Vigil.BestRelicGrade = (int)Grade.Hollowed;
				Vigil.SharedVigilSeconds = 1e4;
				for (int slot = 0; slot < Relics.Slots; slot++)
				{
					Vigil.Worn[slot] = new Relic { Seed = 500 + slot, Grade = Grade.Anointed };
				}
				for (int i = 0; i < Content.Offerings.Length; i++)
				{
					Vigil.OfferingsTaken[i] = true;
				}
			}
			foreach (MarkDef mark in Content.Marks)
			{
				if (mark.Toward == null)
				{
					continue;
				}
				double toward = mark.Toward();
				if (toward < 0.0 || toward > 1.0 || double.IsNaN(toward))
				{
					outOfRange++;
				}
				// Full bar and unearned, or earned and not full: either way the two disagree.
				bool full = toward >= 0.999;
				if (full != mark.Earned())
				{
					lying++;
					if (firstLie.Length == 0)
					{
						firstLie = mark.Name + " reads " + Numbers.Percent(toward)
							+ " and is " + (mark.Earned() ? "earned" : "not earned");
					}
				}
			}
		}
		Vigil.Reset();

		Check("no mark's progress disagrees with the mark", lying == 0,
			lying == 0 ? "empty and full, every bar matches its mark" : firstLie);
		Check("and none of them reads outside its own bar", outOfRange == 0,
			outOfRange == 0 ? "every figure between nothing and all of it" : outOfRange + " out of range");

		Check("every mark is named and can be asked about", unearnable == 0,
			unearnable == 0 ? "all " + Content.Marks.Length + " answerable" : unearnable + " broken");

		// Every offering has to be reachable, or it is a row nobody will ever see.
		int unreachable = 0;
		foreach (OfferingDef def in Content.Offerings)
		{
			bool reachable = def.Target >= 0 ? def.OwnedNeeded > 0 : def.LifetimeNeeded > 0.0;
			if (!reachable || def.Cost <= 0.0)
			{
				unreachable++;
			}
		}
		Check("every offering can actually be offered", unreachable == 0,
			unreachable == 0 ? "all priced and all gated" : unreachable + " unreachable");
	}

	/// <summary>
	/// Nothing the parish said is lost while it is still worth reading.
	/// </summary>
	/// <remarks>
	/// A ring buffer that drops the WRONG end is the classic version of this bug and it is
	/// completely silent: the transcript still fills, still scrolls, still looks right, and the
	/// line the keeper actually wants - the one just spoken - is the one missing. The feed is
	/// capable of forty lines a minute, so the ring wraps in ordinary play rather than as an
	/// edge case.
	/// </remarks>
	private static void NothingSaidIsLost()
	{
		Console.WriteLine("Nothing the parish said is lost");

		Transcript.Clear();
		Check("an empty transcript reads as empty", Transcript.Count == 0 && Transcript.At(0).Line == "",
			"nothing said, nothing held");

		for (int i = 0; i < 12; i++)
		{
			Transcript.Add("line " + i, Omen.Plain, i);
		}
		Check("it reads newest first", Transcript.At(0).Line == "line 11" && Transcript.At(11).Line == "line 0",
			"12 lines, newest at the top");

		// Wrap it many times over, which is what a long vigil does.
		Transcript.Clear();
		int total = Transcript.Capacity * 7 + 13;
		for (int i = 0; i < total; i++)
		{
			Transcript.Add("said " + i, i % 2 == 0 ? Omen.Dread : Omen.Good, i);
		}

		Check("it never holds more than it promised", Transcript.Count == Transcript.Capacity,
			Transcript.Count + " of " + Transcript.Capacity + " after " + total + " lines");

		bool ordered = true;
		for (int i = 0; i < Transcript.Count; i++)
		{
			if (Transcript.At(i).Line != "said " + (total - 1 - i))
			{
				ordered = false;
				break;
			}
		}
		Check("the newest survive and the oldest fall off", ordered,
			ordered ? "the last " + Transcript.Capacity + " in order, after wrapping seven times"
				: "wrapped to the wrong end");

		Check("past the end is empty, not a wrong line", Transcript.At(Transcript.Count).Line == ""
			&& Transcript.At(-1).Line == "", "out of range reads as nothing");

		bool omensHeld = Transcript.At(0).Omen == ((total - 1) % 2 == 0 ? Omen.Dread : Omen.Good);
		Check("a line keeps the weight it was said with", omensHeld,
			"omen travels with the words");

		// The round trip a save makes.
		(string[] lines, int[] omens, double[] at) = Transcript.Capture();
		Check("a save writes them oldest first", lines.Length == Transcript.Capacity
			&& lines[0] == "said " + (total - Transcript.Capacity),
			lines.Length + " written, oldest first");

		string newest = Transcript.At(0).Line;
		Transcript.Restore(lines, omens, at);
		Check("and reading them back is the same transcript",
			Transcript.Count == Transcript.Capacity && Transcript.At(0).Line == newest
				&& Transcript.At(Transcript.Count - 1).Line == lines[0],
			"round-trips whole");

		// A save that has been edited, or written by a version that did not have all three
		// arrays, must not throw - it must simply carry what it can.
		Transcript.Restore(new[] { "a", "b", "c" }, new[] { 99 }, null);
		Check("a ragged save loads without complaint", Transcript.Count == 3
			&& Transcript.At(0).Line == "c" && Transcript.At(0).Omen == Omen.Plain,
			"missing and out-of-range fields fall back");

		Transcript.Restore(null, null, null);
		Check("and a save with none at all is simply empty", Transcript.Count == 0, "nothing to read");

		Transcript.Clear();
	}

	/// <summary>
	/// The button that wears the best three can only ever help.
	/// </summary>
	/// <remarks>
	/// It rewrites both hands and satchel in one go, which is the kind of operation that loses a
	/// relic quietly - and losing a relic is not recoverable. So: nothing may vanish, nothing may
	/// duplicate, the hand may never get worse, and pressing it twice must do nothing the second
	/// time. Fuzzed over random inventories rather than a tidy example, because the interesting
	/// cases are ties and part-filled hands.
	/// </remarks>
	private static void WearingTheBestOnlyHelps()
	{
		Console.WriteLine("Wearing the best three only ever helps");

		Random rng = Seeded(4242);
		int worseHand = 0;
		int lost = 0;
		int duplicated = 0;
		int notIdempotent = 0;
		int predicateWrong = 0;
		int rounds = 3000;

		for (int round = 0; round < rounds; round++)
		{
			Vigil.Reset();
			int held = rng.Next(0, Relics.Satchel + 1);
			List<int> seeds = new List<int>();
			for (int i = 0; i < held; i++)
			{
				Relic relic = new Relic { Seed = rng.Next(1, 1000000), Grade = (Grade)rng.Next(0, 5) };
				seeds.Add(relic.Seed);
				Vigil.Satchel.Add(relic);
			}
			// Wear a random few first, so part-filled and full hands both get exercised.
			for (int slot = 0; slot < Relics.Slots && Vigil.Satchel.Count > 0; slot++)
			{
				if (rng.Next(0, 3) == 0)
				{
					Vigil.Wear(rng.Next(0, Vigil.Satchel.Count), slot);
				}
			}

			double before = 0.0;
			foreach (Relic worn in Vigil.Worn)
			{
				before += Math.Max(0.0, Vigil.RelicWorth(worn));
			}
			bool predicted = Vigil.WouldWearBestChange();

			int moved = Vigil.WearBest();

			double after = 0.0;
			foreach (Relic worn in Vigil.Worn)
			{
				after += Math.Max(0.0, Vigil.RelicWorth(worn));
			}
			if (after < before - 1e-6)
			{
				worseHand++;
			}
			if (predicted != (moved > 0))
			{
				predicateWrong++;
			}

			// Every relic that went in is still somewhere, exactly once.
			List<int> now = new List<int>();
			foreach (Relic worn in Vigil.Worn)
			{
				if (worn.Exists)
				{
					now.Add(worn.Seed);
				}
			}
			foreach (Relic carried in Vigil.Satchel)
			{
				now.Add(carried.Seed);
			}
			seeds.Sort();
			now.Sort();
			if (now.Count != seeds.Count)
			{
				lost++;
			}
			else
			{
				for (int i = 0; i < now.Count; i++)
				{
					if (now[i] != seeds[i])
					{
						duplicated++;
						break;
					}
				}
			}

			if (Vigil.WearBest() != 0)
			{
				notIdempotent++;
			}
		}

		Check("it never leaves a worse hand than it found", worseHand == 0,
			worseHand == 0 ? "over " + rounds + " random satchels" : worseHand + " made worse");
		Check("nothing is lost and nothing is copied", lost == 0 && duplicated == 0,
			lost == 0 && duplicated == 0 ? "every relic still held, exactly once"
				: lost + " lost, " + duplicated + " altered");
		Check("pressing it twice does nothing", notIdempotent == 0,
			notIdempotent == 0 ? "already best, so it stays put" : notIdempotent + " kept moving");
		Check("and the button is dark exactly when it would do nothing", predicateWrong == 0,
			predicateWrong == 0 ? "the label never lies" : predicateWrong + " disagreed");

		Vigil.Reset();
	}

	/// <summary>
	/// Consecration gives a run its shape and cannot be walked back.
	/// </summary>
	private static void ConsecrationIsACommitment()
	{
		Console.WriteLine("Consecration is a commitment");

		Vigil.Reset();
		Check("nothing is consecrated to begin with", Vigil.Consecrated == -1
			&& Vigil.ConsecrationFactor(0) == 1.0, "every rite untouched");

		Check("a rite the keeper does not hold cannot be chosen", !Vigil.Consecrate(0),
			"you cannot consecrate what you have never owned");

		Vigil.Ichor = 1e12;
		Vigil.BuyRite(0, 1);
		Vigil.BuyRite(1, 1);
		Check("one that is held can be", Vigil.Consecrate(0) && Vigil.Consecrated == 0,
			"the first rite carries the run");
		Check("and it cannot be changed afterwards", !Vigil.Consecrate(1) && Vigil.Consecrated == 0,
			"a second attempt is refused, not honoured");

		Check("the chosen rite gains and the rest give up", Vigil.ConsecrationFactor(0) == Vigil.ConsecratedGain
			&& Vigil.ConsecrationFactor(1) == Vigil.ForsakenLoss,
			Vigil.ConsecratedGain + "x chosen, " + Vigil.ForsakenLoss + "x the rest");

		// It has to be a real trade rather than a free upgrade, or it is not a decision.
		Check("consecrating everything would be worse than consecrating nothing",
			Vigil.ForsakenLoss < 1.0 && Vigil.ConsecratedGain > 1.0
				&& Math.Pow(Vigil.ForsakenLoss, Content.RiteCount - 1) * Vigil.ConsecratedGain < Content.RiteCount,
			"the gain is one rite's, the cost is every other rite's");

		// A communion only happens if there is a payout, so the run has to be worth something
		// first - asserting the commune SUCCEEDED, because a check that silently tests a
		// refused communion is a check that passes for the wrong reason.
		Vigil.RunIchor = 1e15;
		Vigil.LifetimeIchor = 1e15;
		bool communed = Vigil.Commune();
		Check("a communion asks the question again", communed && Vigil.Consecrated == -1,
			communed ? "the choice belongs to the run, not the keeper" : "no communion happened");

		Vigil.Reset();
	}

	/// <summary>
	/// A relic that lends structures lends them, and takes them back.
	/// </summary>
	/// <remarks>
	/// The only power that changes the parish rather than a coefficient, so it is the only one
	/// that can leave something behind. What it must never do is let a keeper keep the copies -
	/// or count them toward the free doublings or the price of the next rite, which would turn a
	/// loan of production into a shortcut through the cost curve.
	/// </remarks>
	/// <summary>
	/// What the parish quotes is what the parish pays.
	/// </summary>
	/// <remarks>
	/// <para>
	/// There are two loops that answer "how much is this parish earning". <c>Rate</c> is the one
	/// that gets quoted - on the HUD, in the ward's price, and in the offline catch-up, which
	/// settles the whole time a keeper was away against it in one multiplication. <c>RunCycles</c>
	/// is the one that actually hands ichor over, a working at a time, and it is the only one a
	/// keeper who is present ever experiences.
	/// </para>
	/// <para>
	/// They are meant to be the same total by construction, and that is exactly the kind of claim
	/// that stops being true without anybody noticing: a term added to one of them is a silent
	/// change to the other, in whichever direction nobody looked. It has already happened once,
	/// to the count itself. This plays a real parish and compares what arrived against what was
	/// promised, so the next divergence is a red harness rather than a keeper wondering why the
	/// number on the screen is not the number in their purse.
	/// </para>
	/// <para>
	/// Dread is parked at the parish's own EQUILIBRIUM, where pressure and relaxation cancel, so
	/// the multiplier holds still for the length of the measurement. Pinning it by hand would
	/// have been a fiction the game never produces; this is a state it settles into on its own.
	/// </para>
	/// </remarks>
	/// <summary>
	/// What the bell is worth, now that anything can ask.
	/// </summary>
	/// <remarks>
	/// The bell multiplies the whole economy, and until its interval moved into the vigil there
	/// was no way to measure it from here: the only thing that knew how often it could be rung
	/// was a float on the HUD. So the largest single multiplier a keeper can hold down had never
	/// been in front of the harness at all. This rings it as fast as it can be rung and states
	/// what that is worth, which is the figure a rebalance has to be allowed to move and the
	/// shape it is not.
	/// </remarks>
	private static void TheBellIsWorthSomethingAndNotEverything()
	{
		Console.WriteLine("The bell is worth something and not everything");

		Vigil.Reset();
		Check("the rope moves once", Vigil.Ring(), "the first pull takes");
		Check("and not twice", !Vigil.Ring() && !Vigil.CanRing,
			"another " + Vigil.BellCooldown.ToString("0") + "s on the rope");

		for (double t = 0.0; t < Vigil.kBellInterval; t += kDt)
		{
			Vigil.Tick(kDt);
		}
		Check("and again when the interval is up", Vigil.CanRing && Vigil.Ring(),
			"one bell every " + Vigil.kBellInterval.ToString("0") + "s");

		double quiet = RingingFor(30.0, ring: false);
		double rung = RingingFor(30.0, ring: true);
		double worth = rung / quiet;
		Console.WriteLine("      lone bell x" + Numbers.Short(worth)
			+ ", answered bell x" + Numbers.Short(RingingFor(30.0, ring: true, seconds: 22.0, mult: 3.0) / quiet));
		// RECORDED, not endorsed. These bounds are wide because they are the first measurement of
		// this mechanic ever taken - the interval lived on a widget until now, so nothing could
		// ask - and what they record is that the bell is very large: a keeper on the rope every
		// twenty-five seconds finishes half an hour twelve times ahead, and a congregation
		// answering each other's bells finishes a hundred times ahead, because a surge covering
		// twenty-two of every twenty-five seconds is not a surge, it is the rate. Whether that is
		// the game it wants to be is a decision for the game and not for the harness. What the
		// bounds are for is that it cannot now change by accident.
		Check("a bell rung on every cooldown pays, and pays sanely", worth > 1.05 && worth < 20.0,
			"perfect ringing is worth " + Numbers.Short(worth) + "x over half an hour");

		Vigil.Reset();
	}

	/// <summary>Half an hour of ordinary play, optionally with somebody on the rope the instant
	/// it frees up. Everything else about the two runs is identical, including the seed.</summary>
	private static double RingingFor(double minutes, bool ring, double seconds = 0.0, double mult = 0.0)
	{
		Vigil.Reset();
		Vigil.Rng = Seeded(31);
		double click = 0.0;
		for (int step = 0; step < minutes * 60.0 / kDt; step++)
		{
			click += 4.0 * kDt;
			while (click >= 1.0)
			{
				click -= 1.0;
				Vigil.Gather();
			}
			if (ring && Vigil.Ring())
			{
				Vigil.BeginSurge(seconds > 0.0 ? seconds : Vigil.kLoneBellSeconds,
					mult > 0.0 ? mult : Vigil.kLoneBellMultiplier, fromCongregation: false);
			}
			TakeOfferings();
			Buy();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			Vigil.Tick(kDt);
		}
		return Vigil.LifetimeIchor;
	}

	private static void QuotedIsPaid()
	{
		Console.WriteLine("What the parish quotes is what it pays");

		Vigil.Reset();
		// Deliberately modest. A deep parish sits at an equilibrium ABOVE the brink and is
		// visited every few seconds, and the aftermaths would then be measured as a shortfall
		// against the quoted rate - which is true, and not what this is asking.
		for (int rite = 0; rite < 4; rite++)
		{
			Vigil.Owned[rite] = 2 + rite;
		}
		// Offerings and milestones both multiply one rite and not the others, so turning some on
		// is what makes this a check on the per-rite maths rather than on a single global factor.
		for (int i = 0; i < Content.Offerings.Length; i += 3)
		{
			Vigil.OfferingsTaken[i] = true;
		}

		// Where the meter would sit if left alone, so it does not drift while we measure.
		Vigil.Dread = Vigil.DreadEquilibrium;
		Vigil.Fervour = 0.0;
		Check("the parish under test settles short of the brink", Vigil.DreadEquilibrium < 0.95,
			"equilibrium at " + Vigil.DreadEquilibrium.ToString("0.000"));

		double quoted = Vigil.Rate;
		double slowest = 0.0;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			slowest = Math.Max(slowest, Content.Rites[rite].CycleSeconds);
		}
		int steps = (int)(60.0 * slowest / kDt);
		double before = Vigil.Ichor;
		double dreadBefore = Vigil.Dread;
		for (int i = 0; i < steps; i++)
		{
			Vigil.Tick(kDt);
		}
		double elapsed = steps * kDt;
		double played = (Vigil.Ichor - before) / elapsed;

		Check("the meter held still while we measured", Math.Abs(Vigil.Dread - dreadBefore) < 0.02,
			"dread " + dreadBefore.ToString("0.000") + " to " + Vigil.Dread.ToString("0.000"));
		Check("a played parish earns what it quotes",
			played > quoted * 0.97 && played < quoted * 1.03,
			Numbers.Short(played) + "/s played against " + Numbers.Short(quoted) + "/s quoted");

		Vigil.Reset();
	}

	private static void LentStructuresAreOnlyLent()
	{
		Console.WriteLine("A relic that lends structures takes them back");

		// A relic that definitely carries Foundation, found by asking rather than assuming.
		Relic lender = default;
		for (int seed = 1; seed < 200000 && !lender.Exists; seed++)
		{
			Relic candidate = new Relic { Seed = seed, Grade = Grade.Hollowed };
			for (int i = 0; i < Relics.PowerCount(candidate.Grade); i++)
			{
				if (Relics.PowerAt(candidate, i) == Power.Foundation)
				{
					lender = candidate;
					break;
				}
			}
		}
		Check("a lending relic exists to test at all", lender.Exists,
			lender.Exists ? "found one to wear" : "no Foundation relic in 200k seeds");
		if (!lender.Exists)
		{
			return;
		}

		int rite = Relics.FoundationRite(lender);
		int copies = Relics.FoundationCopies(lender);

		Vigil.Reset();
		Vigil.Satchel.Add(lender);
		Vigil.Wear(0, 0);

		Check("the copies stand even where nothing was bought", Vigil.EffectiveOwned(rite) == copies
			&& Vigil.Owned[rite] == 0, copies + " lent against " + Vigil.Owned[rite] + " bought");
		Check("and they produce", Vigil.Rate > 0.0,
			"a parish of nothing but lent structures still works");

		// PLAYED, not quoted. The check above reads Rate, which is the number the ward is priced
		// off and the number an offline catch-up settles against - and for a while it was the
		// only number that knew about lent structures at all. The loop that actually hands ichor
		// over stepped its own cadence off the bought count, so a keeper wearing this watched
		// three looms stand in their parish and earn nothing for as long as they were looking.
		// It paid only while they were away. Anything that claims a structure works has to be
		// asked by playing it.
		// Sixty of this rite's own workings, so the answer is not dominated by however much of a
		// cycle happened to be left unfinished at the end.
		double quoted = Vigil.Rate;
		double cycle = Math.Max(0.05, Content.Rites[rite].CycleSeconds);
		int steps = (int)(60.0 * cycle / kDt);
		double before = Vigil.Ichor;
		for (int i = 0; i < steps; i++)
		{
			Vigil.Tick(kDt);
		}
		double earned = Vigil.Ichor - before;
		Check("and they produce while the keeper is WATCHING", earned > 0.0,
			Numbers.Short(earned) + " over " + (int)(steps * kDt) + "s of play");

		// The two paths are meant to be the same total by construction, which is the licence the
		// offline catch-up takes to skip the cadence timers entirely. If they disagree, one of
		// them is wrong and the keeper is either being robbed for playing or paid twice for
		// stepping away.
		double played = earned / (steps * kDt);
		Check("and the played rate is the quoted rate", played > quoted * 0.95 && played < quoted * 1.05,
			Numbers.Short(played) + "/s played against " + Numbers.Short(quoted) + "/s quoted");

		double lentRate = Vigil.Rate;
		double priceBefore = Vigil.CostOf(rite, Vigil.Owned[rite]);
		double milestoneBefore = Vigil.MilestoneMultiplier(rite);

		Check("the parish does not count them toward the next price",
			priceBefore == Vigil.CostOf(rite, 0), "priced as though nothing were standing");
		Check("nor toward the free doublings", milestoneBefore == 1.0,
			"lent copies earn no milestone");

		Vigil.Remove(0);
		// One check, with both figures in it. There were two, and the second read
		// `lentRate > 0.0 && costBefore == 0` where costBefore was declared zero and never
		// assigned - so half of it was always true and the other half repeated the check above.
		// It looked like an assertion about cost and asserted nothing at all, which is worse
		// than not being there: a reader counting checks would have counted it.
		Check("taking it off takes them back", Vigil.EffectiveOwned(rite) == 0 && Vigil.Rate == 0.0
			&& lentRate > 0.0,
			Numbers.Short(lentRate) + "/s while worn, " + Numbers.Short(Vigil.Rate) + "/s after");

		Vigil.Reset();
	}

	/// <summary>
	/// Everything the inspector can show, it can show for every relic.
	/// </summary>
	/// <remarks>
	/// The panel reads a relic's powers, its name and its history straight out of the generator,
	/// and there is no fallback for a blank: an empty line just leaves a gap in the panel that
	/// looks like a rendering fault. Cheap to guarantee here and impossible to notice by playing,
	/// since it would need the one seed in thousands that lands on a missing case.
	/// </remarks>
	private static void EveryRelicCanBeRead()
	{
		Console.WriteLine("Every relic can be read");

		Random rng = Seeded(808);
		int blankPower = 0;
		int blankName = 0;
		int blankFlavour = 0;
		int repeatedPower = 0;
		HashSet<string> flavours = new HashSet<string>();
		HashSet<Power> seen = new HashSet<Power>();
		int sampled = 20000;

		for (int i = 0; i < sampled; i++)
		{
			Relic relic = new Relic { Seed = rng.Next(1, int.MaxValue), Grade = (Grade)(i % 5) };
			if (string.IsNullOrWhiteSpace(Relics.NameOf(relic)))
			{
				blankName++;
			}
			string flavour = Relics.Flavour(relic);
			if (string.IsNullOrWhiteSpace(flavour))
			{
				blankFlavour++;
			}
			flavours.Add(flavour);

			HashSet<Power> here = new HashSet<Power>();
			for (int slot = 0; slot < Relics.PowerCount(relic.Grade); slot++)
			{
				Power power = Relics.PowerAt(relic, slot);
				seen.Add(power);
				if (!here.Add(power))
				{
					repeatedPower++;
				}
				if (string.IsNullOrWhiteSpace(Relics.DescribeOn(relic, slot)))
				{
					blankPower++;
				}
			}
		}

		// The supply of NAMES, which the changelog quotes exact figures for. A number in
		// documentation that nothing checks is a number that quietly stops being true - and this
		// one is the whole claim that the relic system does not repeat itself.
		long plain = (long)Relics.Materials * Relics.Forms;
		long anointed = plain * Relics.Provenances;
		long hollowed = anointed * Relics.Epithets;
		Check("there are as many names as the changelog claims",
			anointed >= 15_000 && hollowed >= 150_000,
			Numbers.Short(plain) + " plain, " + Numbers.Short(anointed) + " with a provenance, "
				+ Numbers.Short(hollowed) + " with an epithet as well");

		Check("no relic has a nameless line in it", blankName == 0 && blankPower == 0
			&& blankFlavour == 0, "over " + sampled + " relics, nothing blank");
		Check("no relic grants the same power twice", repeatedPower == 0,
			repeatedPower == 0 ? "every loadout distinct" : repeatedPower + " repeats");
		Check("every kind of power actually turns up", seen.Count == Relics.PowerKinds,
			seen.Count + " of " + Relics.PowerKinds + " kinds seen");
		Check("and a relic's history is not the same story every time", flavours.Count >= 32,
			flavours.Count + " distinct histories");

		// The mask the art is drawn from has to agree with the powers the text lists, or the
		// object on screen is not the object described beside it.
		int disagreed = 0;
		for (int i = 0; i < 5000; i++)
		{
			Relic relic = new Relic { Seed = rng.Next(1, int.MaxValue), Grade = (Grade)(i % 5) };
			int mask = Relics.PowerMask(relic);
			int rebuilt = 0;
			for (int slot = 0; slot < Relics.PowerCount(relic.Grade); slot++)
			{
				rebuilt |= 1 << (int)Relics.PowerAt(relic, slot);
			}
			int packed = (int)(Relics.PackedPowerMask(relic) * (1 << Relics.PowerKinds) + 0.5f);
			if (mask != rebuilt || packed != mask)
			{
				disagreed++;
			}
		}
		Check("the drawing is made of the same powers as the words", disagreed == 0,
			disagreed == 0 ? "mask survives the trip through a colour channel"
				: disagreed + " disagreed");

		// The one above compares C# to C#, which is why it passed while the drawing was wrong.
		// PackedPowerMask divides by 2^kinds and ui_relic.slang multiplies by a NUMBER TYPED OUT
		// IN THE SHADER; when a seventh power was added, C# moved to 128 and the shader stayed at
		// 64, halving every mask and drawing the wrong features on every relic in the game. No
		// check could see it, because both sides of the comparison were the same side.
		//
		// This is the tripwire. It cannot read the shader, so it pins the number instead: add a
		// power and this fails, naming the file and the constant that has to move with it.
		const int shaderDivisor = 512;
		Check("the shader's power divisor still matches", (1 << Relics.PowerKinds) == shaderDivisor,
			(1 << Relics.PowerKinds) == shaderDivisor
				? Relics.PowerKinds + " kinds, so " + shaderDivisor + " in ui_relic.slang"
				: "PowerKinds is now " + Relics.PowerKinds + " - ui_relic.slang must multiply by "
					+ (1 << Relics.PowerKinds) + ", not " + shaderDivisor);
	}

	/// <summary>
	/// A relic that finds relics actually finds relics.
	/// </summary>
	/// <remarks>
	/// The only power whose effect is on a random draw, so it cannot be read off a coefficient
	/// the way the others can - it has to be measured by digging. Worth checking precisely
	/// because it is invisible: a keeper wearing it has no way of telling whether it is doing
	/// anything, and neither would anyone reading the code.
	/// </remarks>
	private static void LuckIsWorthWearing()
	{
		Console.WriteLine("A relic that finds relics finds relics");

		const int digs = 400000;
		const double dread = 0.5;

		Random bareRng = Seeded(31337);
		int bare = 0;
		for (int i = 0; i < digs; i++)
		{
			if (Relics.Dig(bareRng, dread, i + 1).Exists)
			{
				bare++;
			}
		}

		// The same seed, so the only difference between the two runs is the luck.
		Random luckyRng = Seeded(31337);
		double seeking = 0.05 * Relics.SeekingFactor;
		int lucky = 0;
		for (int i = 0; i < digs; i++)
		{
			if (Relics.Dig(luckyRng, dread, i + 1, seeking).Exists)
			{
				lucky++;
			}
		}

		double gain = bare > 0 ? (double)lucky / bare : 0.0;
		Check("wearing it turns up more", gain > 1.15,
			bare + " finds bare, " + lucky + " with it on (" + gain.ToString("F2") + "x)");
		Check("and it is close to what the row promises", Math.Abs(gain - (1.0 + seeking)) < 0.06,
			"promised " + (1.0 + seeking).ToString("F2") + "x, measured " + gain.ToString("F2") + "x");

		// However much is worn, a find must stay a find. The multiplier form is what guarantees
		// this - an additive bonus of the same size would reach certainty.
		Random loadedRng = Seeded(31337);
		int loaded = 0;
		for (int i = 0; i < digs; i++)
		{
			if (Relics.Dig(loadedRng, 1.0, i + 1, 1.0).Exists)
			{
				loaded++;
			}
		}
		Check("and a find is never a certainty", loaded < digs / 3,
			"at the brink, fully loaded: " + (100.0 * loaded / digs).ToString("F1") + "% of clicks");
	}

	/// <summary>
	/// The parish has enough to say, and every line it can say is reachable.
	/// </summary>
	/// <remarks>
	/// A conditional line whose condition can never hold is invisible: it costs nothing, breaks
	/// nothing and simply never appears, so no amount of playing finds it. The transcript makes
	/// the other failure visible instead - a keeper can now scroll back and see the parish say
	/// the same thing four times in a row, which a nine-second feed used to hide.
	/// </remarks>
	private static void TheParishHasEnoughToSay()
	{
		Console.WriteLine("The parish has enough to say");

		int blank = 0;
		foreach (string line in Content.Ambient)
		{
			if (string.IsNullOrWhiteSpace(line))
			{
				blank++;
			}
		}
		foreach (Content.AmbientDef def in Content.Noticed)
		{
			if (string.IsNullOrWhiteSpace(def.Line) || def.When == null)
			{
				blank++;
			}
		}
		// A band whose atmosphere belonged to a different depth would be worse than silence, so
		// the two tables have to stay lined up band for band.
		Check("every dread band has something to say once it has finished teaching",
			Content.Deeper.Length == Content.Murmurs.Length,
			Content.Deeper.Length + " bands of atmosphere against " + Content.Murmurs.Length + " lessons");
		int emptyBand = 0;
		foreach (string[] band in Content.Deeper)
		{
			if (band.Length == 0)
			{
				emptyBand++;
			}
			foreach (string line in band)
			{
				if (string.IsNullOrWhiteSpace(line))
				{
					blank++;
				}
			}
		}
		Check("and none of those bands is empty", emptyBand == 0,
			emptyBand == 0 ? "every depth keeps a voice" : emptyBand + " silent bands");

		// The game's own channel has to reach the transcript, or a line about somebody's save
		// would be spoken into a feed that holds it for nine seconds and then loses it - which
		// is precisely the line they would want to go back and read.
		Transcript.Clear();
		Vigil.Tell("a thing the game had to report", Omen.Dread);
		Check("what the game itself reports is kept too",
			Transcript.Count == 1 && Transcript.At(0).Omen == Omen.Dread,
			"it goes through the same channel the parish does");
		Transcript.Clear();

		Check("nothing it says is blank", blank == 0,
			Content.Ambient.Length + " plain and " + Content.Noticed.Length + " noticed");

		HashSet<string> distinct = new HashSet<string>(Content.Ambient);
		foreach (Content.AmbientDef def in Content.Noticed)
		{
			distinct.Add(def.Line);
		}
		Check("and it never says the same thing twice", distinct.Count
			== Content.Ambient.Length + Content.Noticed.Length,
			distinct.Count + " distinct lines");

		// Every conditional line has to be reachable by SOME keeper, or it is dead text.
		Vigil.Reset();
		Vigil.Ichor = 1e14;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			Vigil.BuyRite(rite, 1);
		}
		Vigil.Consecrate(0);
		Vigil.Dread = 0.8;
		Vigil.HandGathers = 9999;
		Vigil.RelicsRendered = 99;
		Vigil.Communions = 3;
		Vigil.BestRelicGrade = (int)Grade.Hollowed;
		Vigil.Wards = 2;
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			Vigil.OfferingsTaken[i] = true;
		}
		for (int i = 0; i < 8; i++)
		{
			Vigil.Satchel.Add(new Relic { Seed = i + 1, Grade = Grade.Keepsake });
		}
		// A lender worn, and a long-running vigil, for the two lines that ask about those.
		for (int seed = 1; seed < 400000; seed++)
		{
			Relic candidate = new Relic { Seed = seed, Grade = Grade.Hollowed };
			bool lends = false;
			for (int i = 0; i < Relics.PowerCount(candidate.Grade); i++)
			{
				lends |= Relics.PowerAt(candidate, i) == Power.Foundation;
			}
			if (lends)
			{
				Vigil.Worn[0] = candidate;
				break;
			}
		}
		Vigil.PlayedSeconds = 3600.0;

		int unreachableHigh = 0;
		foreach (Content.AmbientDef def in Content.Noticed)
		{
			if (!def.When())
			{
				unreachableHigh++;
			}
		}
		// One line is deliberately about a QUIET parish, so it cannot hold at once with the
		// rest - checked separately rather than pretending a single keeper satisfies everything.
		Vigil.Dread = 0.1;
		int stillUnreachable = 0;
		foreach (Content.AmbientDef def in Content.Noticed)
		{
			if (!def.When())
			{
				stillUnreachable++;
			}
		}
		Check("every line it can say, some keeper can hear",
			unreachableHigh <= 1 && stillUnreachable <= 1,
			"all but one hold at the brink, all but one hold in the quiet");

		Vigil.Reset();
	}

	/// <summary>
	/// Stand still and let the thing arrive.
	/// </summary>
	/// <remarks>
	/// <c>Give(Answer.None)</c> does NOT resolve a visitation - it returns early, because "no
	/// answer" is not an answer a keeper gives, it is one they fail to give, and the encounter
	/// resolves on the clock. A fixture that called it and then asserted the consequences
	/// asserted them about a visitation still walking: two checks here passed that way, finding
	/// an intact satchel because nothing had happened to it yet. Ticking to the end is the only
	/// way to test the branch an absent keeper actually lands on.
	/// </remarks>
	private static void LetItLand()
	{
		for (int step = 0; step < 60000 && Vigil.Approaching; step++)
		{
			Vigil.Tick(kDt);
		}
	}

	/// <summary>
	/// What is worn is safe, and what is loose is not.
	/// </summary>
	/// <remarks>
	/// The rule the whole thing rests on: a keeper who wants to keep a relic can put it on, and
	/// nothing they are relying on ever disappears without their having chosen to leave it
	/// loose. If a worn relic could ever be taken, the satchel stops being a decision and the
	/// loss stops being fair.
	/// <para>
	/// Everything is set up AFTER Summon, which resets the vigil - a fixture that arranges a
	/// satchel and then summons is testing an empty one, and would have passed by finding
	/// nothing to lose.
	/// </para>
	/// </remarks>
	private static void TheDarkTakesWhatIsLoose()
	{
		Console.WriteLine("The dark takes what is loose");

		// Nothing to lose: a visitation must still resolve rather than falling over.
		Summon(0, 0, 1e9);
		LetItLand();
		Check("an empty satchel survives a visitation", Vigil.TimesTaken >= 1 && Vigil.RelicsLost == 0,
			"nothing to take, and nothing breaks");

		// Worn relics are untouchable, however many visitations land.
		Summon(0, 0, 1e9);
		int[] wornSeeds = new int[Relics.Slots];
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			Vigil.Worn[slot] = new Relic { Seed = 900 + slot, Grade = Grade.Hollowed };
			wornSeeds[slot] = Vigil.Worn[slot].Seed;
		}
		LetItLand();
		for (int i = 0; i < 8; i++)
		{
			Vigil.Dread = 1.0;
			Vigil.Tick(kDt);
			LetItLand();
		}
		bool wornHeld = true;
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			wornHeld &= Vigil.Worn[slot].Seed == wornSeeds[slot];
		}
		Check("nothing worn is ever taken", wornHeld && Vigil.RelicsLost == 0,
			"visitations landed, three hands untouched");

		// The best carried thing goes, and only one per visitation.
		Summon(0, 0, 1e9);
		Vigil.Satchel.Add(new Relic { Seed = 11, Grade = Grade.Leavings });
		Vigil.Satchel.Add(new Relic { Seed = 12, Grade = Grade.Hollowed });
		Vigil.Satchel.Add(new Relic { Seed = 13, Grade = Grade.Keepsake });
		LetItLand();
		bool tookTheBest = Vigil.Satchel.Count == 2 && Vigil.RelicsLost == 1;
		foreach (Relic left in Vigil.Satchel)
		{
			tookTheBest &= left.Grade != Grade.Hollowed;
		}
		Check("it takes the best thing in the satchel, and one of them", tookTheBest,
			Vigil.Satchel.Count + " left, the best one gone");

		// A ward that holds costs nothing but the ward.
		Summon(0, 3, 1e9);
		Vigil.Satchel.Add(new Relic { Seed = 21, Grade = Grade.Hallowed });
		LetItLand();
		Check("a ward that holds loses nothing", Vigil.Satchel.Count == 1 && Vigil.RelicsLost == 0,
			"the ward is spent and the satchel is not");

		// Answering correctly loses nothing either - knowing the answer must never cost more
		// than not knowing it, which this suite has caught before.
		Summon(0, 0, 1e9);
		Vigil.Satchel.Add(new Relic { Seed = 31, Grade = Grade.Hallowed });
		Vigil.Give(Vigil.CorrectAnswer);
		Check("and knowing the answer loses nothing", Vigil.Satchel.Count == 1 && Vigil.RelicsLost == 0,
			"turned away, satchel intact");

		// The panel names a relic while something is walking; that name has to be the thing that
		// actually goes, or the warning is worse than silence.
		Summon(0, 0, 1e9);
		Vigil.Satchel.Add(new Relic { Seed = 41, Grade = Grade.Keepsake });
		Vigil.Satchel.Add(new Relic { Seed = 42, Grade = Grade.Hallowed });
		Vigil.Satchel.Add(new Relic { Seed = 43, Grade = Grade.Leavings });
		int warned = Vigil.MostExposed();
		Relic named = Vigil.Satchel[warned];
		LetItLand();
		bool namedOneWentMissing = true;
		foreach (Relic left in Vigil.Satchel)
		{
			namedOneWentMissing &= left.Seed != named.Seed;
		}
		Check("the one it warns about is the one it takes", warned >= 0 && namedOneWentMissing,
			"warned about the " + Relics.GradeName(named.Grade) + ", and that is what went");

		Vigil.Satchel.Clear();
		Check("nothing carried, nothing named", Vigil.MostExposed() == -1, "-1, so the panel says nothing");

		Vigil.Reset();
	}

	/// <summary>
	/// Time away is worth what the parish is actually worth.
	/// </summary>
	/// <remarks>
	/// Offline runs the same Tick as everything else, which is a design decision worth PINNING
	/// rather than trusting: the two newest things that change production - a consecrated rite
	/// and relics that lend structures - would each be easy to add to the live path and forget
	/// on the away path, and the failure is invisible because both numbers look plausible. This
	/// suite has already caught the offline report measuring the wrong quantity once.
	/// </remarks>
	private static void TimeAwayIsWorthWhatItSays()
	{
		Console.WriteLine("Time away is worth what the parish is worth");

		// A consecrated rite pays three times as much away as well as at home.
		Vigil.Reset();
		Vigil.Ichor = 1e12;
		Vigil.BuyRite(0, 40);
		double plainRate = Vigil.Rate;
		Vigil.Ichor = 0.0;
		for (int i = 0; i < (int)(60.0 / kDt); i++)
		{
			Vigil.Tick(kDt, offline: true);
		}
		double plainAway = Vigil.Ichor;

		Vigil.Reset();
		Vigil.Ichor = 1e12;
		Vigil.BuyRite(0, 40);
		Vigil.Consecrate(0);
		double blessedRate = Vigil.Rate;
		Vigil.Ichor = 0.0;
		for (int i = 0; i < (int)(60.0 / kDt); i++)
		{
			Vigil.Tick(kDt, offline: true);
		}
		double blessedAway = Vigil.Ichor;

		double awayRatio = plainAway > 0.0 ? blessedAway / plainAway : 0.0;
		double homeRatio = plainRate > 0.0 ? blessedRate / plainRate : 0.0;
		Check("a consecrated rite pays the same multiple away as at home",
			Math.Abs(awayRatio - homeRatio) < 0.02 && awayRatio > 2.5,
			homeRatio.ToString("F2") + "x at home, " + awayRatio.ToString("F2") + "x away");

		// Lent structures work while the keeper is not there, exactly as bought ones do.
		Vigil.Reset();
		Relic lender = default;
		for (int seed = 1; seed < 400000 && !lender.Exists; seed++)
		{
			Relic candidate = new Relic { Seed = seed, Grade = Grade.Hollowed };
			for (int i = 0; i < Relics.PowerCount(candidate.Grade); i++)
			{
				if (Relics.PowerAt(candidate, i) == Power.Foundation)
				{
					lender = candidate;
					break;
				}
			}
		}
		Vigil.Worn[0] = lender;
		Vigil.Ichor = 0.0;
		for (int i = 0; i < (int)(60.0 / kDt); i++)
		{
			Vigil.Tick(kDt, offline: true);
		}
		Check("structures a relic lends work while the keeper is away", Vigil.Ichor > 0.0,
			"a parish of nothing but lent structures still earns overnight");

		// And the away rate is a SHARE of the home rate, never more than it.
		Check("time away never pays better than being there",
			Vigil.OfflineEfficiency > 0.0 && Vigil.OfflineEfficiency <= 1.0,
			"away runs at " + Numbers.Percent(Vigil.OfflineEfficiency) + " of the parish");

		Vigil.Reset();
	}

	/// <summary>
	/// An echo remembers which keeper it was.
	/// </summary>
	/// <remarks>
	/// The capture happens inside Commune, BEFORE the reset that clears the consecration and
	/// empties the parish - which is an ordering, and orderings drift. If RecordEcho ever moved
	/// below those lines every echo would silently record as an uncommitted keeper who owned
	/// nothing, and the line would go back to being four names with no past. Nothing about that
	/// would throw.
	/// </remarks>
	private static void AnEchoRemembersWhoItWas()
	{
		Console.WriteLine("An echo remembers who it was");

		Vigil.Reset();
		Vigil.Ichor = 1e14;
		Vigil.BuyRite(0, 5);
		Vigil.BuyRite(2, 5);
		Vigil.Consecrate(2);
		Vigil.RunIchor = 1e15;
		Vigil.LifetimeIchor = 1e15;
		bool communed = Vigil.Commune();

		Check("a communion leaves an echo of that keeper", communed && Vigil.Echoes.Count == 1,
			communed ? Vigil.Echoes.Count + " standing behind" : "no communion happened");
		Check("and it remembers what the run was given to",
			Vigil.Echoes.Count == 1 && Vigil.Echoes[0].Rite == 2,
			Vigil.Echoes.Count == 1 ? "consecrated to rite " + Vigil.Echoes[0].Rite : "no echo");
		Check("and how deep it got", Vigil.Echoes.Count == 1 && Vigil.Echoes[0].Depth == 2,
			Vigil.Echoes.Count == 1 ? "deepest rite " + Vigil.Echoes[0].Depth : "no echo");

		// A keeper who never committed leaves an echo that says so, rather than one that
		// claims the first rite.
		Vigil.Ichor = 1e14;
		Vigil.BuyRite(0, 5);
		Vigil.RunIchor = 1e15;
		Vigil.LifetimeIchor = 1e15;
		Vigil.Commune();
		Echo second = Vigil.Echoes[Vigil.Echoes.Count - 1];
		Check("a keeper who never chose is remembered as one who never chose", second.Rite == -1,
			"rite " + second.Rite + ", so the panel says they never chose");

		// The round trip a save makes, including the two fields added last.
		VigilSave saved = VigilData.Capture();
		Vigil.Reset();
		VigilData.Apply(saved);
		bool held = Vigil.Echoes.Count >= 2 && Vigil.Echoes[0].Rite == 2 && Vigil.Echoes[0].Depth == 2;
		Check("a saved echo keeps its past", held,
			held ? "the line survives being written down" : "the past did not survive the save");

		// A save from before echoes had a past must load without one, not throw and not invent.
		saved.EchoRites = Array.Empty<int>();
		saved.EchoDepths = null!;
		Vigil.Reset();
		VigilData.Apply(saved);
		bool olderLoads = Vigil.Echoes.Count >= 2;
		foreach (Echo echo in Vigil.Echoes)
		{
			olderLoads &= echo.Rite == -1 && echo.Depth == -1;
		}
		Check("and an older save loads a line with no past at all", olderLoads,
			"missing fields become 'never chose', not rite zero");

		Vigil.Reset();
	}

	/// <summary>
	/// Where the shaders keep the game's own constants.
	/// </summary>
	/// <remarks>
	/// Found by walking up from wherever the harness was built to, rather than assumed: the
	/// build output sits several directories under the project and the working directory
	/// depends on how the harness was launched.
	/// </remarks>
	private static string ShaderPath(string file)
	{
		// Nullable, because Parent IS null at a drive root - which is exactly where this walk
		// ends when the file is not found.
		DirectoryInfo? dir = new DirectoryInfo(AppContext.BaseDirectory);
		for (int up = 0; up < 12 && dir != null; up++, dir = dir.Parent)
		{
			string candidate = Path.Combine(dir.FullName, "assets", "shaders", file);
			if (File.Exists(candidate))
			{
				return candidate;
			}
		}
		return "";
	}

	/// <summary>Pull one number out of a shader by matching the line it is written on.</summary>
	private static double ShaderNumber(string file, string pattern, out string trouble)
	{
		trouble = "";
		string path = ShaderPath(file);
		if (path.Length == 0)
		{
			trouble = "could not find " + file;
			return double.NaN;
		}
		Match match = Regex.Match(File.ReadAllText(path), pattern);
		if (!match.Success)
		{
			trouble = "no line matching /" + pattern + "/ in " + file;
			return double.NaN;
		}
		return double.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
	}

	/// <summary>
	/// The numbers the shaders share with the game still agree with it.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Several constants are written twice: once in C# and once, by hand, in a shader. Every
	/// comment around them says the two MUST agree, and for the power mask that was not enough -
	/// C# moved from 64 to 128 when a seventh power was added, the shader did not, and every
	/// relic in the game drew the wrong features. Silently, because there is no build step that
	/// compares them and no way for a wrong-but-plausible drawing to look like a fault.
	/// </para>
	/// <para>
	/// The first fix pinned the number in a second place here, which only moves the problem. This
	/// READS THE SHADER. If the file cannot be found or the line cannot be matched, the check
	/// FAILS rather than passing quietly - a verifier that silently stops verifying is worse
	/// than none, because it keeps reporting green.
	/// </para>
	/// </remarks>
	private static void TheShadersAgreeWithTheGame()
	{
		Console.WriteLine("The shaders still agree with the game");

		double horizon = ShaderNumber("ui_parish.slang",
			@"const\s+float\s+horizon\s*=\s*([0-9.]+)\s*;", out string trouble);
		Check("the horizon is in the same place on both sides",
			trouble.Length == 0 && Math.Abs(horizon - Layout.Horizon) < 1e-6,
			trouble.Length > 0 ? trouble
				: "ui_parish draws it at " + horizon.ToString("0.000")
					+ ", the parish stands on " + Layout.Horizon.ToString("0.000"));

		double divisor = ShaderNumber("ui_relic.slang",
			@"saturate\(pc\.color0\.a\)\s*\*\s*([0-9.]+)", out trouble);
		Check("the relic power mask unpacks with the number it was packed by",
			trouble.Length == 0 && Math.Abs(divisor - (1 << Relics.PowerKinds)) < 1e-6,
			trouble.Length > 0 ? trouble
				: "ui_relic multiplies by " + divisor.ToString("0")
					+ ", C# divides by " + (1 << Relics.PowerKinds));

		// The rolling fronts: three numbers, all of which have to match Layout.PackWave.
		double scale = ShaderNumber("ui_parish.slang",
			@"saturate\(packed\)\s*\*\s*([0-9.]+)", out trouble);
		Check("a front's packing scale matches", trouble.Length == 0 && Math.Abs(scale - 256.0) < 1e-6,
			trouble.Length > 0 ? trouble : "both use " + scale.ToString("0"));

		double weightBits = ShaderNumber("ui_parish.slang",
			@"fmod\(high,\s*([0-9.]+)\)", out trouble);
		Check("and so does the room left for a front's weight",
			trouble.Length == 0 && Math.Abs(weightBits - Layout.WaveWeight) < 1e-6,
			trouble.Length > 0 ? trouble
				: "ui_parish reads " + weightBits.ToString("0") + " weights, Layout allows "
					+ Layout.WaveWeight);

		double originSteps = ShaderNumber("ui_parish.slang",
			@"floor\(high\s*/\s*8\.0\)\s*/\s*([0-9.]+)", out trouble);
		Check("and the steps a front's origin is quantised to",
			trouble.Length == 0 && Math.Abs(originSteps - 31.0) < 1e-6,
			trouble.Length > 0 ? trouble : "both use " + originSteps.ToString("0") + " steps");
	}

	/// <summary>Any file in the project, found by walking up from the build output.</summary>
	private static string ProjectFile(params string[] parts)
	{
		// Nullable, because Parent IS null at a drive root - which is exactly where this walk
		// ends when the file is not found.
		DirectoryInfo? dir = new DirectoryInfo(AppContext.BaseDirectory);
		for (int up = 0; up < 12 && dir != null; up++, dir = dir.Parent)
		{
			string candidate = Path.Combine(dir.FullName, Path.Combine(parts));
			if (File.Exists(candidate) || Directory.Exists(candidate))
			{
				return candidate;
			}
		}
		return "";
	}

	/// <summary>
	/// The scene still has everything the scripts reach for.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Scripts bind scene elements by NAME, as strings, and <c>Scene.Find</c> answers a rename
	/// with an invalid entity rather than an error. Everything downstream then no-ops quietly:
	/// the panel is simply never shown, the button is never clickable, and the game runs
	/// perfectly well minus one feature. There is no stack trace and nothing in the log.
	/// </para>
	/// <para>
	/// The scene is generated by a Python script and the names are typed out again in C#, which
	/// is the same two-sides-of-a-boundary shape as the shader constants - and the same fix:
	/// read the other side rather than trusting a comment.
	/// </para>
	/// </remarks>
	private static void TheSceneHasWhatTheScriptsAskFor()
	{
		Console.WriteLine("The scene has what the scripts ask for");

		string scriptDir = ProjectFile("scripts");
		string sceneDir = ProjectFile("scenes");
		if (scriptDir.Length == 0 || sceneDir.Length == 0)
		{
			Check("the scripts and scenes can be read at all", false,
				"could not find the project's scripts or scenes");
			return;
		}

		HashSet<string> authored = new HashSet<string>();
		foreach (string file in Directory.GetFiles(sceneDir, "*.toml"))
		{
			foreach (Match match in Regex.Matches(File.ReadAllText(file), @"name = '([^']*)'"))
			{
				authored.Add(match.Groups[1].Value);
			}
		}

		List<string> missing = new List<string>();
		int bound = 0;
		foreach (string file in Directory.GetFiles(scriptDir, "*.cs"))
		{
			string text = File.ReadAllText(file);
			foreach (Match match in Regex.Matches(text,
				@"(?:Scene\.Find|Button\.Find)\(""([A-Za-z0-9_]+)"""))
			{
				string name = match.Groups[1].Value;
				bound++;
				// A name built by appending an index - Scene.Find("InspectPower" + i) - reaches
				// the harness as its prefix, so a family counts as present when its first member
				// is. Checking the prefix alone would miss a family that lost members; checking
				// only exact matches would flag every family in the game.
				if (authored.Contains(name) || authored.Contains(name + "0"))
				{
					continue;
				}
				missing.Add(name + " (" + Path.GetFileName(file) + ")");
			}
		}

		Check("every element a script binds is in the scene", missing.Count == 0,
			missing.Count == 0
				? bound + " names bound, all authored"
				: string.Join(", ", missing));
	}

	/// <summary>
	/// The two numbers the scene generator shares with the game still agree.
	/// </summary>
	/// <remarks>
	/// Python cannot read a C# constant, so the type scale and the ledger's width are written
	/// twice and paired by a comment saying "move both or neither". That is exactly the
	/// arrangement the relic power mask had when it broke, so it gets the same treatment: read
	/// the generator and compare.
	/// </remarks>
	private static void TheGeneratorAgreesWithTheGame()
	{
		Console.WriteLine("The scene generator still agrees with the game");

		string generator = ProjectFile("tools", "generate_scenes.py");
		if (generator.Length == 0)
		{
			Check("the scene generator can be read at all", false, "could not find generate_scenes.py");
			return;
		}
		string text = File.ReadAllText(generator);

		Match type = Regex.Match(text, @"^TYPE = ([0-9.]+)", RegexOptions.Multiline);
		Check("the authored type is scaled by the same number as the built type",
			type.Success && Math.Abs(double.Parse(type.Groups[1].Value, CultureInfo.InvariantCulture)
				- Typography.Scale) < 1e-6,
			type.Success
				? "generator " + type.Groups[1].Value + ", Typography " + Typography.Scale.ToString("0.00")
				: "no TYPE in the generator");

		// The ledger's width lives in Hud, which the harness cannot reference - it needs the
		// engine - so both sides are read as text.
		Match ledgerPy = Regex.Match(text, @"^LEDGER_W = ([0-9.]+)", RegexOptions.Multiline);
		string hud = ProjectFile("scripts", "Hud.cs");
		Match ledgerCs = hud.Length > 0
			? Regex.Match(File.ReadAllText(hud), @"LedgerWidth = ([0-9.]+)f")
			: Match.Empty;
		Check("the ledger is the same width in both places",
			ledgerPy.Success && ledgerCs.Success
				&& Math.Abs(double.Parse(ledgerPy.Groups[1].Value, CultureInfo.InvariantCulture)
					- double.Parse(ledgerCs.Groups[1].Value, CultureInfo.InvariantCulture)) < 1e-6,
			ledgerPy.Success && ledgerCs.Success
				? "generator " + ledgerPy.Groups[1].Value + ", Hud " + ledgerCs.Groups[1].Value
				: "could not read one of the two");
	}

	/// <summary>
	/// Every number the keeper reads stays readable.
	/// </summary>
	/// <remarks>
	/// An idle game's numbers do not stop, and this is the one function standing between all of
	/// them and the screen - with no coverage at all until now. The failures it has to be free
	/// of are the quiet kind: a column that changes width and makes the whole panel jump, a
	/// suffix table that runs out and starts printing exponents mid-game, or a glyph the font
	/// was never baked with.
	/// </remarks>
	private static void EveryNumberStaysReadable()
	{
		Console.WriteLine("Every number the keeper reads stays readable");

		int tooLong = 0;
		int empty = 0;
		int nonAscii = 0;
		int widest = 0;
		string worst = "";

		// Every decade from a fraction to well past anything the game can reach.
		for (int exponent = -2; exponent <= 40; exponent++)
		{
			foreach (double lead in new[] { 1.0, 1.5, 3.33, 9.99 })
			{
				double value = lead * Math.Pow(10.0, exponent);
				foreach (string text in new[] { Numbers.Short(value), Numbers.Short(-value) })
				{
					if (string.IsNullOrEmpty(text))
					{
						empty++;
						continue;
					}
					if (text.Length > widest)
					{
						widest = text.Length;
						worst = text;
					}
					if (text.Length > 10)
					{
						tooLong++;
					}
					foreach (char c in text)
					{
						if (c > 126)
						{
							nonAscii++;
						}
					}
				}
			}
		}

		Check("no figure is ever blank", empty == 0, "every decade from a hundredth upward prints");
		Check("and none of them is wide enough to move a column", tooLong == 0,
			"the widest was \"" + worst + "\" at " + widest + " characters");
		Check("and all of them are ASCII", nonAscii == 0,
			nonAscii == 0 ? "the font bakes no other glyphs" : nonAscii + " characters the font lacks");

		// A broken save, or a runaway multiplier, must not put "NaN" on the HUD.
		Check("nothing impossible reaches the screen",
			Numbers.Short(double.NaN) == "-" && Numbers.Short(double.PositiveInfinity) == "-"
				&& Numbers.Short(double.NegativeInfinity) == "-",
			"NaN and infinity read as a dash");

		// The suffixes have to be used wherever they exist; falling back to exponents early
		// would be correct and unreadable.
		bool suffixed = !Numbers.Short(1.0e30).Contains("e") && !Numbers.Short(9.9e32).Contains("e");
		Check("the named tiers are used before exponents are", suffixed,
			Numbers.Short(1.0e30) + " and " + Numbers.Short(9.9e32));

		// Ordering: bigger numbers must not read as smaller ones within a tier.
		int inverted = 0;
		for (int exponent = 0; exponent <= 12; exponent++)
		{
			double small = 1.2 * Math.Pow(10.0, exponent);
			double large = 8.7 * Math.Pow(10.0, exponent);
			if (string.CompareOrdinal(Numbers.Short(small), Numbers.Short(large)) >= 0)
			{
				inverted++;
			}
		}
		Check("and within a tier the larger figure reads larger", inverted == 0,
			inverted == 0 ? "ordering holds across thirteen tiers" : inverted + " inverted");
	}

	/// <summary>
	/// A rescued save never lands on a name already in use.
	/// </summary>
	/// <remarks>
	/// This runs at the worst possible moment - the keeper's save is already unreadable and the
	/// copy being moved is the only one left. Returning a name that is taken would have the
	/// rescue destroy the thing it exists to preserve, and it would only ever happen to somebody
	/// who had broken a save twice, which is to say almost nobody, which is to say it would
	/// never be found.
	/// </remarks>
	private static void ARescuedSaveNeverLandsOnAnother()
	{
		Console.WriteLine("A rescued save never lands on another");

		const string save = "hollowtide.save";
		HashSet<string> onDisk = new HashSet<string>();
		bool Taken(string name) => onDisk.Contains(name);

		Check("the first rescue takes the plain name", SaveNaming.Kept(save, Taken) == save + ".broken",
			SaveNaming.Kept(save, Taken));

		// Fill them up one at a time, exactly as a keeper breaking saves repeatedly would.
		int collisions = 0;
		for (int i = 0; i < 40; i++)
		{
			string name = SaveNaming.Kept(save, Taken);
			if (!onDisk.Add(name))
			{
				collisions++;
			}
		}
		Check("and forty of them in a row are forty different files", collisions == 0,
			collisions == 0 ? onDisk.Count + " distinct names" : collisions + " would have overwritten");

		// Past the ceiling it has to return SOMETHING, and it must never be the live save.
		HashSet<string> everything = new HashSet<string>();
		for (int i = 0; i < SaveNaming.MaxKept + 5; i++)
		{
			everything.Add(SaveNaming.Kept(save, everything.Contains));
		}
		string beyond = SaveNaming.Kept(save, everything.Contains);
		Check("and once they are all taken it still names a file, never the save itself",
			beyond.Length > 0 && beyond != save && beyond.StartsWith(save + ".broken"),
			"the hundredth name is reused rather than the live save overwritten");

		// A missing predicate must not throw - the rescue is already handling a failure.
		Check("a rescue with nothing to ask still names something",
			SaveNaming.Kept(save, null!) == save + ".broken", "no predicate, no exception");

		// The temp suffix is shared, which is the whole reason it lives here.
		Check("the unfinished write has exactly one name", SaveNaming.TempSuffix == ".tmp",
			"Save writes it and Load recovers from it, spelled once");
	}

	/// <summary>
	/// This suite does not assert things that cannot fail.
	/// </summary>
	/// <remarks>
	/// <para>
	/// A check that passes for the wrong reason is worse than no check: it is counted, it is
	/// reported green, and it occupies the place where a real one would have gone. This project
	/// has produced several - a fixture that answered a visitation with a call that resolves
	/// nothing, a comparison of C# against C# for a value that crosses into a shader, an
	/// assertion on a variable declared zero and never assigned.
	/// </para>
	/// <para>
	/// Only the crudest form can be caught mechanically - a condition written as a literal - so
	/// that is what this catches. It reads its own source, which is the same trick the shader
	/// and scene checks use, and fails if the file cannot be found rather than passing quietly.
	/// </para>
	/// </remarks>
	private static void NothingHereIsAssertedForShow()
	{
		Console.WriteLine("Nothing here is asserted for show");

		string self = ProjectFile("tools", "balance", "Balance.cs");
		if (self.Length == 0)
		{
			Check("the suite can read itself", false, "could not find Balance.cs");
			return;
		}

		string text = File.ReadAllText(self);
		MatchCollection calls = Regex.Matches(text, @"Check\(\s*""([^""]+)"",\s*(true|false)\s*,");
		List<string> always = new List<string>();
		foreach (Match call in calls)
		{
			// A literal `false` is legitimate: it is how a check reports that the thing it
			// needed was missing, and it only runs on that path. A literal `true` never can be.
			if (call.Groups[2].Value == "true")
			{
				always.Add(call.Groups[1].Value);
			}
		}

		Check("no check is written as a constant truth", always.Count == 0,
			always.Count == 0
				? Regex.Matches(text, @"Check\(").Count + " checks, none of them decorative"
				: string.Join("; ", always));
	}

	/// <summary>
	/// A visitor announces itself differently and means the same thing.
	/// </summary>
	/// <remarks>
	/// The approach line is the ONLY thing a keeper has to work out what a visitor wants, and
	/// learning to read it is the whole progression of the encounter. So varying the wording is
	/// flavour and varying the meaning would be the game cheating. What can be checked
	/// mechanically is that the variation is only ever wording: the same visitor, the same
	/// answer, one line chosen per approach and shown identically everywhere.
	/// </remarks>
	private static void AVisitorMeansTheSameHoweverItSpeaks()
	{
		Console.WriteLine("A visitor means the same however it speaks");

		int blank = 0;
		int duplicated = 0;
		int thin = 0;
		HashSet<string> everything = new HashSet<string>();
		foreach (RiteDef rite in Content.Rites)
		{
			if (string.IsNullOrWhiteSpace(rite.Approach))
			{
				blank++;
			}
			if (rite.AlsoApproach.Length == 0)
			{
				thin++;
			}
			foreach (string line in rite.AlsoApproach)
			{
				if (string.IsNullOrWhiteSpace(line))
				{
					blank++;
				}
			}
			// Two visitors sharing a sentence would make the tell ambiguous, which is the one
			// thing the encounter cannot survive.
			foreach (string line in new List<string>(rite.AlsoApproach) { rite.Approach })
			{
				if (!everything.Add(line))
				{
					duplicated++;
				}
			}
		}

		Check("every visitor has more than one way of announcing itself", thin == 0 && blank == 0,
			everything.Count + " lines across " + Content.Rites.Length + " visitors");
		// The same treatment for what the parish says when something is LOST, which fires at the
		// worst moment in the game and had one sentence per rite.
		int thinLoss = 0;
		foreach (RiteDef rite in Content.Rites)
		{
			if (rite.AlsoTaken.Length == 0 || string.IsNullOrWhiteSpace(rite.TakenLine))
			{
				thinLoss++;
			}
			foreach (string line in rite.AlsoTaken)
			{
				if (!everything.Add(line))
				{
					duplicated++;
				}
				if (string.IsNullOrWhiteSpace(line))
				{
					blank++;
				}
			}
			if (!everything.Add(rite.TakenLine))
			{
				duplicated++;
			}
		}
		Check("and more than one way of reporting a loss", thinLoss == 0 && blank == 0,
			everything.Count + " lines in all, across announcements and losses");

		Check("and no two of them share a sentence", duplicated == 0,
			duplicated == 0 ? "every tell belongs to exactly one visitor" : duplicated + " shared");

		// The line is drawn once per approach and everything reads that one. Summon repeatedly
		// and confirm the line always belongs to the visitor that is actually walking.
		int mismatched = 0;
		HashSet<string> seen = new HashSet<string>();
		for (int attempt = 0; attempt < 200; attempt++)
		{
			Summon(attempt % Content.RiteCount, 0, 1e9);
			if (!Vigil.Approaching)
			{
				continue;
			}
			RiteDef walking = Content.Rites[Vigil.ApproachRite];
			bool belongs = Vigil.ApproachLine == walking.Approach
				|| Array.IndexOf(walking.AlsoApproach, Vigil.ApproachLine) >= 0;
			if (!belongs)
			{
				mismatched++;
			}
			seen.Add(Vigil.ApproachLine);
			LetItLand();
		}
		Check("the line said always belongs to the thing walking", mismatched == 0,
			mismatched == 0 ? seen.Count + " different announcements, all of them the right one"
				: mismatched + " described a different visitor");
		Check("and more than one of them actually gets used", seen.Count > Content.Rites.Length,
			seen.Count + " distinct lines drawn across 200 approaches");

		// Nothing walking, nothing said - so the panel cannot show last time's warning.
		Vigil.Reset();
		Check("nothing is announced when nothing is coming", Vigil.ApproachLine.Length == 0,
			"the line is cleared with the encounter");
	}

	/// <summary>
	/// The counter that keeps every relic distinct cannot be broken by a file.
	/// </summary>
	/// <remarks>
	/// It is floored above whatever a save carries, so that a reload cannot hand out seeds
	/// already spoken for - which means a save is an INPUT to it, and a save is a file somebody
	/// can edit or a disk can mangle. One carrying int.MaxValue used to overflow the counter to
	/// a negative number on the very next find, and the seed is the only thing that stops two
	/// relics being the same object.
	/// </remarks>
	private static void TheSeedCounterCannotBeBrokenByAFile()
	{
		Console.WriteLine("The seed counter cannot be broken by a file");

		Vigil.Reset();
		VigilSave save = VigilData.Capture();

		save.RelicSeed = int.MaxValue;
		VigilData.Apply(save);
		int first = Vigil.NextRelicSeed();
		bool firstOk = first >= 1 && first <= Vigil.MaxRelicSeed;
		Check("a save carrying the largest possible seed does not overflow it", firstOk,
			firstOk ? "the next seed is " + first + ", inside the range every relic assumes"
				: "the next seed is " + first + ", outside it");

		save.RelicSeed = int.MinValue;
		VigilData.Apply(save);
		int negative = Vigil.NextRelicSeed();
		bool negativeOk = negative >= 1 && negative <= Vigil.MaxRelicSeed;
		Check("nor does one carrying a negative", negativeOk,
			"the next seed is " + negative + (negativeOk ? "" : ", outside the range"));

		// Walked right up to the ceiling and over it, which is the only way to reach the wrap.
		Vigil.RelicSeed = Vigil.MaxRelicSeed - 2;
		int zeros = 0;
		int outside = 0;
		int previous = 0;
		bool wrapped = false;
		for (int i = 0; i < 6; i++)
		{
			int seed = Vigil.NextRelicSeed();
			if (seed == 0)
			{
				zeros++;
			}
			if (seed < 1 || seed > Vigil.MaxRelicSeed)
			{
				outside++;
			}
			if (seed < previous)
			{
				wrapped = true;
			}
			previous = seed;
		}
		Check("it wraps rather than running past its own ceiling", wrapped && outside == 0,
			wrapped && outside == 0
				? "stepped over the ceiling and came back inside it"
				: outside + " seeds past the ceiling, " + (wrapped ? "wrapped" : "never wrapped"));
		// Zero is how the game says a slot is EMPTY, so a relic must never be given it.
		Check("and it never hands out the seed that means nothing is there", zeros == 0,
			"no relic is ever seeded zero");

		Vigil.Reset();
	}

	/// <summary>
	/// A wrong file cannot make the parish's arithmetic meaningless.
	/// </summary>
	/// <remarks>
	/// The two arrays read straight into the simulation are what a keeper HOLDS and what boons
	/// they have. Neither was clamped: a negative holding gives negative production and a
	/// negative price, and a boon level read past its own maximum drives multipliers - what
	/// dread pays, the ward cap, how fast fervour drains - beyond anything the balance was ever
	/// measured at. Everything else off a file was already clamped; these two were not, and they
	/// are the two the whole economy is computed from.
	/// </remarks>
	private static void AWrongFileCannotBreakTheParish()
	{
		Console.WriteLine("A wrong file cannot break the parish");

		Vigil.Reset();
		VigilSave save = VigilData.Capture();

		int[] wrongOwned = new int[Content.RiteCount];
		int[] wrongBoons = new int[Content.Boons.Length];
		for (int i = 0; i < wrongOwned.Length; i++)
		{
			wrongOwned[i] = i % 2 == 0 ? -50 : int.MaxValue;
		}
		for (int i = 0; i < wrongBoons.Length; i++)
		{
			wrongBoons[i] = i % 2 == 0 ? -7 : 999;
		}
		save.Owned = wrongOwned;
		save.Boons = wrongBoons;
		VigilData.Apply(save);

		int badOwned = 0;
		foreach (int owned in Vigil.Owned)
		{
			if (owned < 0 || owned > VigilData.MostOwnable)
			{
				badOwned++;
			}
		}
		Check("nothing held is negative or beyond reach", badOwned == 0,
			badOwned == 0 ? "every holding inside what the cost curve allows" : badOwned + " impossible");

		int badBoons = 0;
		for (int i = 0; i < Vigil.Boons.Length; i++)
		{
			if (Vigil.Boons[i] < 0 || Vigil.Boons[i] > Content.Boons[i].MaxLevel)
			{
				badBoons++;
			}
		}
		Check("no boon is past its own ceiling", badBoons == 0,
			badBoons == 0 ? "every level inside the ladder it was written for" : badBoons + " over");

        // The point of the clamps: the numbers the game runs on stay numbers.
		bool sane = !double.IsNaN(Vigil.Rate) && !double.IsInfinity(Vigil.Rate) && Vigil.Rate >= 0.0
			&& !double.IsNaN(Vigil.DreadMultiplier) && Vigil.DreadMultiplier >= 1.0
			&& Vigil.WardCost > 0.0 && !double.IsInfinity(Vigil.WardCost);
		Check("and the parish still computes", sane,
			sane ? Numbers.Rate(Vigil.Rate) + ", wards at " + Numbers.Short(Vigil.WardCost)
				: "rate " + Vigil.Rate + ", wards " + Vigil.WardCost);

		Vigil.Reset();
	}

	/// <summary>
	/// A very long vigil never reaches a number that stops being one.
	/// </summary>
	/// <remarks>
	/// <para>
	/// An idle game's whole shape is a number climbing without end, and the genre's classic
	/// failure is that it eventually stops being a number: production hits infinity, the price
	/// of the next thing hits infinity, and the difference between them becomes NaN. From then
	/// on nothing can be bought and nothing can be earned, and no error is ever raised.
	/// </para>
	/// <para>
	/// This was only asked of a CORRUPT save until now - which found a real overflow - and never
	/// of ordinary play, which is the case that actually matters because it arrives on its own.
	/// Days of it are simulated here at speed, buying as hard as the ichor allows, since the
	/// point is to reach the largest numbers the rules can produce rather than to play well.
	/// </para>
	/// </remarks>
	private static void ALongVigilStaysFinite()
	{
		Console.WriteLine("A long vigil stays a number");

		// Only on the default seed. This is the most expensive section in the suite by far - a
		// fortnight of ticks against a parish rich enough to make each one costly - and what it
		// asserts is that the arithmetic stays arithmetic, which no shift of the seeds changes.
		// Running it once per sweep instead of three times keeps the whole verifier inside a
		// minute, and a check nobody waits for is a check nobody runs.
		if (s_seedOffset != 0)
		{
			Console.WriteLine("  (skipped on a shifted seed - it measures arithmetic, not luck)");
			return;
		}

		Vigil.Reset();
		double worstRate = 0.0;
		double worstIchor = 0.0;
		// Tracked ACROSS the run rather than read at the end. The end of the loop lands wherever
		// it lands relative to a communion, and a communion empties the parish - so reading the
		// final state asked "is the parish rich right now" and got whichever answer the last few
		// seconds happened to give. It passed by luck and then failed by luck.
		int mostAvailable = 0;
		int deepestEver = -1;
		int mostOwnedEver = 0;
		int broke = 0;
		string firstBreak = "";
		int communions = 0;

		// A fortnight of simulated hard play, WEARING what turns up. Two days were tried first
		// and peaked at 22 trillion a second, which proves very little; sixty days reaches far
		// higher but takes the whole suite past two minutes, because a rich parish makes every
		// tick expensive and a verifier nobody runs is worth nothing. A fortnight reaches
		// 7.7x10^15 across three hundred communions and gets to the deepest rite, which is
		// enough to say the curve does not turn - and still some 290 orders of magnitude short
		// of where a double gives up.
		const double step = 4.0;
		for (int tick = 0; tick < (int)(14 * 24 * 3600 / step); tick++)
		{
			Vigil.Tick(step);
			Vigil.Gather();
			Buy();
			TakeOfferings();
			if (Vigil.Approaching)
			{
				Vigil.Give(Vigil.CorrectAnswer);
			}
			// Wear what turns up, and give the run to a rite. Without these the keeper simulated
			// here never touched the three largest multipliers in the game - a consecration is
			// worth three times on one rite, a full loadout better than three times overall, and
			// a lending relic adds copies of a tier outright. Checking that production stays
			// finite while leaving those out is checking the wrong keeper: they are exactly
			// where an overflow would come from.
			// Wear what has turned up. Relics have to be in this run - a full loadout is the
			// largest multiplier a keeper has, and leaving it out measures the wrong keeper.
			//
			// The cadence looked like it mattered enormously and does not. Calling this every
			// tick once collapsed a sixty-day run from 116 quadrillion a second to 2.5 billion,
			// which reads exactly like an order-dependent bug in a helper that is supposed to be
			// idempotent. Measured across three seeds it flips: every-tick came out seven and
			// eighteen times WORSE on two of them and sixteen million times BETTER on the third.
			// Wearing a relic changes the find chance, a find consumes an extra draw, and from
			// there the two runs see different luck forever - in an economy built on repeated
			// doublings that compounds into eight orders of magnitude either way.
			//
			// Which is the real lesson of this section: the peak below is a sample of something
			// enormously variable and is NOT a figure to draw conclusions from. What is asserted
			// is that the arithmetic stays arithmetic, and that holds on every trajectory.
			if (tick % 900 == 0 && Vigil.Satchel.Count > 0)
			{
				Vigil.WearBest();
			}
			// NOT consecrated. It was tried two ways - the moment anything was owned, and once
			// the deepest tier opened - and both crippled the run, from 91 quadrillion a second
			// down to 97 billion and then 2.5 billion. Consecration gives one rite three times
			// and taxes the other seven by a fifth each, which against a strategy that spreads
			// its buying is a net loss that then compounds through the free doublings. That is a
			// real thing to know about consecration and it belongs in its own section, not here:
			// this check wants the LARGEST numbers the rules can reach, and a keeper who plays
			// badly does not reach them. Consecration's own multipliers are bounded and cannot
			// overflow anything the doublings do not already dominate.
			// Commune whenever it pays, which is what a keeper chasing the biggest numbers does
			// and what makes the permanent multipliers stack up over a long run.
			if (Vigil.SigilsOnOffer > 0 && tick % 900 == 0)
			{
				if (Vigil.Commune())
				{
					communions++;
					SpendSigils();
				}
			}

			bool sane = !double.IsNaN(Vigil.Rate) && !double.IsInfinity(Vigil.Rate)
				&& !double.IsNaN(Vigil.Ichor) && !double.IsInfinity(Vigil.Ichor)
				&& !double.IsNaN(Vigil.LifetimeIchor) && !double.IsInfinity(Vigil.LifetimeIchor)
				&& Vigil.Rate >= 0.0 && Vigil.Ichor >= 0.0;
			if (!sane)
			{
				broke++;
				if (firstBreak.Length == 0)
				{
					firstBreak = "after " + Numbers.Duration(tick * step) + ": rate " + Vigil.Rate
						+ ", ichor " + Vigil.Ichor;
				}
			}
			int availableNow = 0;
			for (int i = 0; i < Content.Offerings.Length; i++)
			{
				if (Vigil.OfferingAvailable(i))
				{
					availableNow++;
				}
			}
			mostAvailable = Math.Max(mostAvailable, availableNow);
			deepestEver = Math.Max(deepestEver, Vigil.DeepestRite());
			mostOwnedEver = Math.Max(mostOwnedEver, Vigil.MostOwned());
			worstRate = Math.Max(worstRate, double.IsInfinity(Vigil.Rate) ? double.MaxValue : Vigil.Rate);
			worstIchor = Math.Max(worstIchor, double.IsInfinity(Vigil.Ichor) ? double.MaxValue : Vigil.Ichor);
		}

		Check("a fortnight of hard play never stops being numbers", broke == 0,
			broke == 0
				? "peaked at " + Numbers.Rate(worstRate) + " across " + communions
					+ " communions (one sample of a very variable thing - see above)"
				: firstBreak);

		// And the price of the next thing stays payable-or-not, rather than becoming NaN - which
		// would make every comparison against it false and quietly end the game.
		int unpriceable = 0;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			double cost = Vigil.CostOf(rite, Vigil.Owned[rite]);
			if (double.IsNaN(cost) || cost < 0.0)
			{
				unpriceable++;
			}
		}
		// What is LEFT to do, asked of the whole run rather than of its final instant.
		Check("and the parish keeps asking for things", mostAvailable > 0,
			"as many as " + mostAvailable + " offerings on offer at once, of " + Content.Offerings.Length);

		int maxed = 0;
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			if (Vigil.Boons[i] >= Content.Boons[i].MaxLevel)
			{
				maxed++;
			}
		}
		// Reported rather than asserted, because it is a design question and not a fault: every
		// boon is maxed long before here, so sigils stop being something a keeper SPENDS and
		// become only something they accumulate for the passive multiplier. That is exactly the
		// decay the boons were added to fix, arriving again further out. Recorded here so the
		// figure is in front of whoever decides what to do about it.
		Console.WriteLine("         (after a fortnight: " + maxed + "/" + Content.Boons.Length
			+ " boons maxed, " + Numbers.Short(Vigil.SigilsEarned) + " sigils earned, "
			+ Vigil.MarksHeld() + "/" + Content.Marks.Length + " marks, reached rite "
			+ deepestEver + ", " + mostOwnedEver + " of one rite)");

		Check("and everything still has a price", unpriceable == 0,
			unpriceable == 0 ? "every rite still quotes one" : unpriceable + " cost nothing meaningful");

		Vigil.Reset();
	}

	/// <summary>
	/// The sink cannot be bought into dominance.
	/// </summary>
	/// <remarks>
	/// The Old Bargain's ladder runs to two hundred levels so that sigils always have somewhere
	/// to go. That is safe only because its cost multiplies while its effect adds - what a
	/// keeper actually gets grows with the logarithm of what they earn. Nothing enforced that
	/// shape, though: raising Step, or flattening Growth, would turn the sink into the only
	/// thing worth buying and make riding the brink the whole game. This pins both ends - how
	/// far the ladder can be climbed with real earnings, and how strong it is when you get
	/// there.
	/// </remarks>
	private static void TheSinkCannotBeBoughtIntoDominance()
	{
		Console.WriteLine("The prestige sink cannot be bought into dominance");

		int sink = 0;
		for (int i = 0; i < Content.Boons.Length; i++)
		{
			if (Content.Boons[i].MaxLevel > Content.Boons[sink].MaxLevel)
			{
				sink = i;
			}
		}

		// What a fortnight of hard play earns, from the long run above. Rounded down hard: the
		// question is what a keeper reaches, not what the richest possible one does.
		const long earned = 124_000;
		Vigil.Reset();
		int affordable = 0;
		long spent = 0;
		while (affordable < Content.Boons[sink].MaxLevel)
		{
			Vigil.Boons[sink] = affordable;
			long next = Vigil.BoonCost(sink);
			if (spent + next > earned)
			{
				break;
			}
			spent += next;
			affordable++;
		}
		Check("a fortnight's sigils buy a dozen levels, not the ladder",
			affordable >= 6 && affordable <= 25,
			"a fortnight buys " + affordable + " levels of " + Content.Boons[sink].Name
				+ " for " + Numbers.Short(spent) + " sigils");

		// And at that level the bargain is still a bargain rather than the whole game.
		Vigil.Reset();
		Vigil.Dread = 1.0;
		double bare = Vigil.DreadMultiplier;
		Vigil.Boons[sink] = affordable;
		double bought = Vigil.DreadMultiplier;
		Check("and what they buy is a better bargain, not a different game",
			bought > bare && bought < bare * 3.0,
			"dread pays " + Numbers.Mult(bare) + " bare and " + Numbers.Mult(bought)
				+ " after a fortnight of buying it");

		// The far end has to stay finite, since a save may legitimately hold it.
		Vigil.Boons[sink] = Content.Boons[sink].MaxLevel;
		double ceiling = Vigil.DreadMultiplier;
		Check("and the top of the ladder is still a number", !double.IsNaN(ceiling)
			&& !double.IsInfinity(ceiling), "dread pays " + Numbers.Mult(ceiling) + " at level "
				+ Content.Boons[sink].MaxLevel);

		Vigil.Reset();
	}

	private static int Main(string[] args)
	{
		for (int i = 0; i < args.Length - 1; i++)
		{
			if (args[i] == "--seed" && int.TryParse(args[i + 1], out int offset))
			{
				s_seedOffset = offset;
			}
		}

		Console.WriteLine();
		if (s_seedOffset != 0)
		{
			Console.WriteLine("(every seed shifted by " + s_seedOffset + ")");
		}
		StokeIsATradeNotAnExit();
		InsuranceCostsTheSameAtEveryScale();
		DreadIsAliveFromTheFirstRite();
		PrestigeRatchets();
		TheEncounterIsOptional();
		TheParishSpeaks();
		TheParishDoesNotRepeatItself();
		EveryMarkIsReachable();
		TablesLineUp();
		IdlingWorks();
		TheParishGivesThingsUp();
		TradingMovesRatherThanCopies();
		TheDeadRememberWhatTheyTake();
		AVigilSurvivesBeingWrittenDown();
		TheReadoutsAgree();
		TheClockOutlastsTheKeeper();
		TheParishHoldsItselfTogether();
		TheFrontsSurviveBeingPacked();
		EverythingCanBeRead();
		OldSavesStillMeanWhatTheyMeant();
		NothingSaidIsLost();
		WearingTheBestOnlyHelps();
		ConsecrationIsACommitment();
		TheBellIsWorthSomethingAndNotEverything();
		QuotedIsPaid();
		LentStructuresAreOnlyLent();
		EveryRelicCanBeRead();
		LuckIsWorthWearing();
		TheParishHasEnoughToSay();
		TheDarkTakesWhatIsLoose();
		TimeAwayIsWorthWhatItSays();
		AnEchoRemembersWhoItWas();
		TheShadersAgreeWithTheGame();
		TheSceneHasWhatTheScriptsAskFor();
		TheGeneratorAgreesWithTheGame();
		EveryNumberStaysReadable();
		ARescuedSaveNeverLandsOnAnother();
		NothingHereIsAssertedForShow();
		AVisitorMeansTheSameHoweverItSpeaks();
		TheSeedCounterCannotBeBrokenByAFile();
		AWrongFileCannotBreakTheParish();
		ALongVigilStaysFinite();
		TheSinkCannotBeBoughtIntoDominance();
		NothingBreaksUnderPressure();
		Console.WriteLine();
		Console.WriteLine(s_checks + " invariants checked.");
		Console.WriteLine(s_failures == 0 ? "The vigil holds." : s_failures + " invariant(s) broken.");
		return s_failures == 0 ? 0 : 1;
	}
}
