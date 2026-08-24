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
		Console.WriteLine();
		Console.WriteLine(s_failures == 0 ? "The vigil holds." : s_failures + " invariant(s) broken.");
		return s_failures == 0 ? 0 : 1;
	}
}
