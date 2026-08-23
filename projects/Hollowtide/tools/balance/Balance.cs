using System;

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

	private static void Check(string what, bool ok, string detail)
	{
		Console.WriteLine("  [" + (ok ? "PASS" : "FAIL") + "] " + what.PadRight(46) + " " + detail);
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

	private static void TakeOfferings()
	{
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
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
		Check("riding high costs uptime", patient.ReelingFraction is > 0.05 and < 0.5,
			"reeling " + (patient.ReelingFraction * 100.0).ToString("0") + "% of the run");
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

		int spendable = 0;
		Vigil.Reset();
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
		Check("a first communion cannot buy the game out", firstOffer * 8 < spendable,
			"first run offers " + firstOffer + " against " + spendable + " to spend");
		Check("a first communion still buys something", firstOffer >= 1,
			firstOffer + " sigils after half an hour");
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
			Vigil.Sigils = 1000000;
			maxedRefuses &= Vigil.BoonCost(i) == 0 && !Vigil.BuyBoon(i);
		}
		Check("a maxed boon cannot be bought again", maxedRefuses, "all six refuse");

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
		// The one promise offline progress makes: it cannot cost you anything you were not
		// there to defend.
		Check("offline never provokes a visitation", Vigil.TimesTaken == 0, "nothing arrives while away");
	}

	private static int Main()
	{
		Console.WriteLine();
		StokeIsATradeNotAnExit();
		InsuranceCostsTheSameAtEveryScale();
		DreadIsAliveFromTheFirstRite();
		PrestigeRatchets();
		TablesLineUp();
		IdlingWorks();
		Console.WriteLine();
		Console.WriteLine(s_failures == 0 ? "The vigil holds." : s_failures + " invariant(s) broken.");
		return s_failures == 0 ? 0 : 1;
	}
}
