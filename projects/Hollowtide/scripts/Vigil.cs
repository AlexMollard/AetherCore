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

	/// <summary>
	/// Wards in hand. Each one stands between the keeper and a visitation, and is spent doing
	/// it. Capped at <see cref="MaxWards"/>.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Wards used to be a one-shot half-step down the meter, which made them a REACTION TIMER:
	/// at a mid parish a ward bought 19 seconds before the next was due and at a full parish
	/// 12, because the climb scaled with holdings while the decay was a flat constant. Progress
	/// made the game more annoying and the only correct play was to sit and press a button.
	/// </para>
	/// <para>
	/// The first attempt at a fix had the ward hold the climb BACK, which was worse: dread is
	/// the thing that pays, so suppressing it meant a ward was a purchase that made you poorer,
	/// and simulating it confirmed no rational keeper would ever buy one. A ward is insurance
	/// now. It does not keep you away from the top of the meter - it lets you STAY there, which
	/// is where you want to be, and it is spent when the dark arrives. Its real price is not
	/// the ichor, it is that the ichor did not go on the next rite.
	/// </para>
	/// </remarks>
	public static int Wards;

	/// <summary>How many wards can be held. A small buffer, not a stockpile you can hide
	/// behind indefinitely.</summary>
	public const int MaxWards = 3;

	/// <summary>
	/// Seconds left of the aftermath of a visitation, during which the parish works at half
	/// pace. This is what a visitation COSTS now.
	/// </summary>
	/// <remarks>
	/// It used to take 35% of banked ichor. Saving up for the next tier is the whole activity
	/// of an idle game, so taxing the bank punished the player for playing properly, and it
	/// punished them hardest at exactly the moment they were closest to a breakthrough. A
	/// stretch of halved production costs time instead of stock: it scales with how good the
	/// parish is without anyone tuning it, and it can never undo a purchase or a save-up.
	/// </remarks>
	public static double AftermathSeconds;

	/// <summary>
	/// 0 to 1. Built by gathering with your own hands, and it drains when you stop.
	/// </summary>
	/// <remarks>
	/// The reason to touch the screen. Hand gathering was worth a twentieth of a second of
	/// production per click, so there was never a point to it once the first rite was up.
	/// Fervour makes clicking worth something WITHOUT making it mandatory: idle play simply
	/// runs at the base rate, which is what the whole economy is tuned against.
	/// </remarks>
	public static double Fervour;

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

	/// <summary>
	/// Raised the moment the dark arrives. The argument is whether a ward HELD it: the two
	/// outcomes are opposite in meaning and must not look the same, or buying insurance would
	/// be indistinguishable from being caught without it.
	/// </summary>
	public static Action<bool>? OnVisitation;

	/// <summary>Bumped whenever a purchase, a loss or a communion changes what the ledger
	/// should show. Panels rebuild on a change rather than every frame.</summary>
	public static int Revision;

	/// <summary>How far through its current working each rite is, 0 to 1. Read by the ledger
	/// and by the parish, so the player can watch the thing they bought doing its job.</summary>
	public static readonly double[] CycleProgress = new double[Content.RiteCount];

	/// <summary>A rite completed a working and delivered this much. The presentation layer
	/// turns it into a flare and a mote travelling to the purse; nothing here knows that.</summary>
	public static Action<int, double>? OnYield;

	/// <summary>
	/// Fired whenever the keeper spends on ANYTHING: a rite (with its index), or an offering,
	/// a mark or a communion (index -1).
	/// </summary>
	/// <remarks>
	/// The hook lives here, on the one type that takes the payment, rather than at each of the
	/// four call sites that trigger a purchase. Spending and the parish answering are the same
	/// event, so a new thing to buy should not be able to be added without the scene noticing -
	/// which is exactly what happened to offerings, marks and communions, all of which took the
	/// money and changed nothing you could see.
	/// </remarks>
	public static Action<int>? OnSpent;

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
			double mult = SigilMultiplier * DreadMultiplier * CongregationBonus * FervourMultiplier;
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
	/// <summary>
	/// What dread pays. Steeper than linear on purpose.
	/// </summary>
	/// <remarks>
	/// At 1 + 1.5d the whole meter was worth 2.5x at the very top, so the difference between
	/// playing it safe at 0.3 and riding the edge at 0.9 was 1.45x against 2.35x - not enough
	/// to be worth a visitation, which made the only sane play "keep it low" and turned the
	/// central mechanic into pure downside. Curving it puts most of the reward in the top
	/// third, so choosing to sit up there is a real strategy with a real prize.
	/// </remarks>
	public static double DreadMultiplier => 1.0 + 2.6 * Math.Pow(Dread, 1.4);

	/// <summary>Ichor per second from rites alone, after any aftermath.</summary>
	public static double Rate => RawRate * AftermathScale;

	/// <summary>Production before a visitation's aftermath is applied. The ward's price is
	/// quoted off this, so being hit does not also make recovering cheaper.</summary>
	public static double RawRate
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

	/// <summary>Half while a visitation's aftermath is running, otherwise full.</summary>
	public static double AftermathScale => AftermathSeconds > 0.0 ? 0.5 : 1.0;

	/// <summary>How fast fervour drains, per second.</summary>
	public const double kFervourDrain = 0.085;

	/// <summary>What one hand gather adds to fervour. Roughly three seconds of steady
	/// clicking to fill it, and about twelve seconds of not clicking to lose it.</summary>
	public const double kFervourPerGather = 0.075;

	/// <summary>What fervour is worth at the top: a little over half again.</summary>
	public static double FervourMultiplier => 1.0 + Fervour * 0.6;

	/// <summary>How fast dread falls on its own, per second.</summary>
	/// <remarks>
	/// An idle game must not punish idling. Dread that only ever climbs turns "leave it
	/// running" - the thing the whole genre is built on - into the losing move, and makes a
	/// button you have to come back and press the price of playing at all. With decay, a
	/// parish left alone settles at whatever level its holdings sustain and stays there. A
	/// visitation becomes something you walk INTO by reaching for the multiplier, never
	/// something that happens because you looked away.
	/// </remarks>
	public const double DreadDecay = 0.004;

	/// <summary>The pressure the parish puts on the meter, before the ward holds any of it
	/// back. Owning more raises this; it is the reason a big parish is a dangerous one.</summary>
	public static double DreadPressure
	{
		get
		{
			double total = 0.0;
			for (int i = 0; i < Content.RiteCount; i++)
			{
				total += Owned[i] * Content.Rites[i].DreadRate;
			}
			// A late run would otherwise pin the meter within seconds. Owning more of a tier
			// should raise dread; owning ALL the tiers should not make the game unplayable,
			// so the climb is compressed rather than summed flat.
			return Math.Sqrt(total) * 0.022;
		}
	}

	/// <summary>What the meter climbs at. Wards do not touch this - they catch what happens
	/// when it arrives at the top.</summary>
	public static double DreadRate => DreadPressure;

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
	/// <summary>What a ward costs. Around a tenth of what the parish makes between visitations,
	/// so keeping insured is a visible drag on growth without being a tax you cannot pay.</summary>
	public static double WardCost => 40.0 + RawRate * 12.0;

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
		Fervour = Math.Min(1.0, Fervour + kFervourPerGather);
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
		OnSpent?.Invoke(rite);
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
		OnSpent?.Invoke(-1);
		return true;
	}

	/// <summary>Spend to set the ward standing again, holding the climb back while it lasts.</summary>
	public static bool RaiseWard()
	{
		double cost = WardCost;
		if (cost > Ichor || Wards >= MaxWards)
		{
			return false;
		}
		Ichor -= cost;
		Wards++;
		WardsRaised++;
		Revision++;
		Say("A ward is set aside. It will stand between you once.", Omen.Good);
		return true;
	}

	/// <summary>
	/// Reach for the dark deliberately: a step up the meter, for the multiplier it pays.
	/// </summary>
	/// <remarks>
	/// This is what makes dread a CHOICE rather than a timer. Decay means the meter drifts
	/// down on its own, so the only way to sit high enough to be worth it - and close enough
	/// to be dangerous - is to keep putting yourself there.
	/// </remarks>
	public static bool Stoke()
	{
		if (Dread >= 0.999)
		{
			return false;
		}
		Dread = Math.Min(1.0, Dread + 0.18);
		// It PAYS. Stoking used to add risk and nothing else, which made it a button with no
		// argument for pressing it - the multiplier it bought arrived slowly and could be had
		// by waiting. Taking the offer up front is what makes this a trade rather than a dare.
		double offered = (Rate * 15.0 + HandGain * 8.0) * GlobalMultiplier;
		Ichor += offered;
		RunIchor += offered;
		LifetimeIchor += offered;
		Say("You lean closer. It gives you " + Numbers.Short(offered) + " and remembers.", Omen.Dread);
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
		OnSpent?.Invoke(-1);
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

		if (AftermathSeconds > 0.0)
		{
			AftermathSeconds = Math.Max(0.0, AftermathSeconds - deltaSeconds);
		}
		// Fervour drains steadily, so it is a reward for playing NOW rather than a level you
		// grind once and keep.
		Fervour = Math.Max(0.0, Fervour - kFervourDrain * deltaSeconds);

		double efficiency = offline ? 0.5 : 1.0;
		if (offline)
		{
			// Catching up on hours does not need eight cycle timers stepped thousands of
			// times; the continuous rate is the same total by construction.
			double gained = Rate * deltaSeconds * efficiency;
			Ichor += gained;
			RunIchor += gained;
			LifetimeIchor += gained;
		}
		else
		{
			RunCycles(deltaSeconds);
		}
		PlayedSeconds += deltaSeconds;

		// Net of decay, so a parish tends toward an equilibrium rather than a cliff.
		Dread = Math.Clamp(Dread + (DreadRate - DreadDecay) * deltaSeconds * efficiency, 0.0, 1.0);
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

	/// <summary>
	/// Advance every rite's working and pay out the ones that finish.
	/// </summary>
	/// <remarks>
	/// The economics are identical to a per-second trickle - a working pays exactly what the
	/// rite would have earned over its cadence - but it arrives as an EVENT. That is the
	/// whole difference between owning a number and owning a thing: you bought a loom, and
	/// now you can watch the loom finish a bolt and hand it over.
	/// </remarks>
	private static void RunCycles(double deltaSeconds)
	{
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			if (Owned[rite] <= 0)
			{
				CycleProgress[rite] = 0.0;
				continue;
			}

			double cycle = Math.Max(0.05, Content.Rites[rite].CycleSeconds);
			CycleProgress[rite] += deltaSeconds / cycle;
			// A long frame, or a surge, can finish more than one working; pay for each so a
			// stutter never eats production.
			while (CycleProgress[rite] >= 1.0)
			{
				CycleProgress[rite] -= 1.0;
				double yield = Owned[rite] * Content.Rites[rite].BaseRate * RiteMultiplier(rite) * cycle * AftermathScale;
				Ichor += yield;
				RunIchor += yield;
				LifetimeIchor += yield;
				OnYield?.Invoke(rite, yield);
			}
		}
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

	/// <summary>
	/// The meter filled. Something takes a share of the ichor you have not spent yet, and
	/// leaves the dread most of the way down.
	/// </summary>
	/// <remarks>
	/// It takes the PURSE, never the parish. Losing rites is unrecoverable progress loss for
	/// a player who stepped away from a game designed to be stepped away from; losing unspent
	/// ichor is a setback the next few minutes of idling repair. The tension survives - you
	/// still lose something you wanted - and the punishment now lands on the player who
	/// pushed their luck rather than the one who went to make a coffee.
	/// </remarks>
	private static void Visitation()
	{
		Revision++;

		// A ward stands in the way and is spent doing it. Dread falls back only part way, so
		// being insured leaves the keeper HIGH on the meter and still earning - which is the
		// entire reason to have bought one.
		if (Wards > 0)
		{
			Wards--;
			Dread = 0.55;
			Say("A ward takes it. The parish does not notice, and you do.", Omen.Good);
			OnVisitation?.Invoke(true);
			return;
		}

		Dread = 0.0;
		AftermathSeconds = kAftermath;
		TimesTaken++;

		int rite = DeepestRite();
		string flavour = rite >= 0 ? Content.Rites[rite].TakenLine : "Something walks the empty parish, and finds only you.";
		Say(flavour + "  The parish works at half pace for " + (int)kAftermath + "s.", Omen.Taken);
		OnVisitation?.Invoke(false);
	}

	/// <summary>How long a parish is left reeling. Long enough to hurt, short enough that the
	/// answer is to keep playing rather than to put the game down.</summary>
	private const double kAftermath = 30.0;

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
		Array.Clear(CycleProgress, 0, CycleProgress.Length);
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
