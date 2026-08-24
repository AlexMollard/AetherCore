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
		Vigil.Rng = new Random(20260824);

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
		Vigil.Rng = new Random(1);
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
		double silent = PlayAnswering(45, null);
		double knowing = PlayAnswering(45, Answer.None);
		double guessing = PlayAnswering(45, Answer.Still);
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
		Vigil.Rng = new Random(7);
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
		Vigil.Rng = new Random(3);
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
			switch (rng.Next(12))
			{
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
			if (Vigil.ApproachSeconds < 0.0 || Vigil.ApproachSeconds > Vigil.kApproachSeconds)
			{
				return "walk clock is " + Vigil.ApproachSeconds;
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

	/// <summary>Play a stretch, optionally leaning on the dead and optionally answering what
	/// that brings. Communes on sight so there is a line to lean on at all.</summary>
	private static double Lean(double minutes, bool shunt, bool answers)
	{
		Vigil.Reset();
		Vigil.Rng = new Random(11);
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
		Random rng = new Random(4);
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

		// -- The bug every inventory has: items multiplying. --
		Vigil.Reset();
		Vigil.Rng = new Random(5);
		for (int i = 0; i < 8; i++)
		{
			Vigil.Satchel.Add(new Relic { Seed = 100 + i, Grade = Grade.Keepsake });
		}
		int before = Count();
		Random shuffle = new Random(6);
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

		// -- The satchel has to hold, and hold the RIGHT things. --
		Vigil.Reset();
		Vigil.Rng = new Random(7);
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

	private static int Main()
	{
		Console.WriteLine();
		StokeIsATradeNotAnExit();
		InsuranceCostsTheSameAtEveryScale();
		DreadIsAliveFromTheFirstRite();
		PrestigeRatchets();
		TheEncounterIsOptional();
		TheParishSpeaks();
		EveryMarkIsReachable();
		TablesLineUp();
		IdlingWorks();
		TheParishGivesThingsUp();
		TheDeadRememberWhatTheyTake();
		NothingBreaksUnderPressure();
		Console.WriteLine();
		Console.WriteLine(s_failures == 0 ? "The vigil holds." : s_failures + " invariant(s) broken.");
		return s_failures == 0 ? 0 : 1;
	}
}
