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
	/// <summary>Index into <see cref="Content.Boons"/>. Named rather than numbered because
	/// every one of these is read from exactly one formula, and a table reordered by hand
	/// would otherwise silently rewire six of them at once.</summary>
	public enum Boon
	{
		DeeperWards,
		ColdBlood,
		OldBargain,
		SteadyHand,
		Unsleeping,
		QuickKindling,
	}

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

	/// <summary>Levels held in each of <see cref="Content.Boons"/>. Kept through communion,
	/// like the sigils that bought them.</summary>
	public static int[] Boons = new int[Content.Boons.Length];

	/// <summary>
	/// The keepers this one used to be, and what each of them is carrying for you.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The congregation, for a keeper who has none. Pushing your dread onto somebody else is
	/// the most distinctive thing this game does and it needed a second player online to do
	/// it, so in the sessions almost everybody actually plays, it did not exist. A communion
	/// leaves a keeper behind - the game opens on "the last keeper left the ledger open" - and
	/// those are the ones standing close enough to take something from you.
	/// </para>
	/// <para>
	/// <b>It is a loan, not a bin.</b> Shedding dread at will would end the game: dread is the
	/// whole bargain, the meter filling is the only thing that threatens anybody, and a free
	/// dump button removes the risk while keeping the reward. So what an echo takes it KEEPS,
	/// and while it is carrying it the line as a whole lets go of dread more slowly - see
	/// <see cref="DreadRelax"/>. You do not get rid of anything. You borrow against the dead
	/// and the interest is a meter that never settles as low again.
	/// </para>
	/// </remarks>
	public static readonly List<Echo> Echoes = new List<Echo>();

	/// <summary>What the keeper is carrying but not wearing.</summary>
	public static readonly List<Relic> Satchel = new List<Relic>();

	/// <summary>What the keeper has on them. A relic in a slot is the only kind that does
	/// anything - carrying one is not wearing it.</summary>
	public static readonly Relic[] Worn = new Relic[Relics.Slots];

	/// <summary>Relics ever dug out of the parish, for the record.</summary>
	public static int RelicsFound;

	/// <summary>
	/// Seeds the digger has already used.
	/// </summary>
	/// <remarks>
	/// A counter rather than a random draw, so one keeper never digs up the same relic twice -
	/// but the counter STARTS somewhere random, which matters as soon as relics can be traded.
	/// Starting every keeper at zero means every keeper's fifth find is the same relic as every
	/// other keeper's fifth find, so handing somebody a relic would routinely hand them a
	/// duplicate of something already in their satchel, and the whole point of trading is that
	/// what arrives is something you could not have dug up yourself.
	/// </remarks>
	public static int RelicSeed;

	/// <summary>How many keepers stand close enough to be asked. Matches the roster the
	/// congregation panel already draws, so the same four rows serve both.</summary>
	public const int MaxEchoes = 4;

	/// <summary>Sigils in hand, to spend. Spending these does NOT weaken the keeper - see
	/// <see cref="SigilsEarned"/>.</summary>
	public static int Sigils;

	/// <summary>
	/// Every sigil ever taken, spent or not. What the permanent multiplier is drawn from.
	/// </summary>
	/// <remarks>
	/// Split from <see cref="Sigils"/> because one number cannot be both a balance and a
	/// score. While the multiplier read the BALANCE, buying anything with sigils made the
	/// keeper measurably worse at the game, so the prestige loop was a treadmill: simulated
	/// over ten communions, the tenth run reached less than the first. A prestige currency has
	/// to ratchet or there is no reason to prestige, and the moment there is something to
	/// spend it on, the balance stops being able to carry the ratchet.
	/// </remarks>
	public static int SigilsEarned;

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
	/// behind indefinitely - though a keeper who has communed for it can carry a little more.</summary>
	public static int MaxWards => kBaseMaxWards + (int)(BoonLevel(Boon.DeeperWards) * Content.Boons[(int)Boon.DeeperWards].Step);

	private const int kBaseMaxWards = 3;

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


	// ── Records, kept for the marks and the ledger ───────────────────────────────────

	public static double PlayedSeconds;
	public static double HighDreadSeconds;
	public static int WardsRaised;
	public static int TimesTaken;
	/// <summary>Visitors turned away by naming what they wanted. The record of the one kind of
	/// progress this game keeps in the player rather than in the save.</summary>
	public static int VisitorsAnswered;

	/// <summary>Which visitors this keeper has met, and which they have turned away. The
	/// second is what the ledger will admit it knows the answer to: a visitor you have beaten
	/// once is one you have PROVED you can name, and writing it down is the difference between
	/// a mechanic you learn and a mechanic you look up in a wiki.</summary>
	public static bool[] VisitorsMet = new bool[Content.RiteCount];
	public static bool[] VisitorsBested = new bool[Content.RiteCount];
	public static int CommunionSurges;
	public static int HandGathers;
	public static double SharedVigilSeconds;

	// ── Live state, not saved ────────────────────────────────────────────────────────

	/// <summary>
	/// Which rite's visitor is currently walking toward the keeper, or -1 for nothing.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The encounter, and the reason the game is not a spreadsheet wearing a horror skin. A
	/// visitation used to RESOLVE the instant the meter filled: a ward went, or the parish
	/// worked at half pace, and either way the player watched a number change. Every rite
	/// carried a beautifully written line about something walking off with what you owned,
	/// attached to an event with no decision anywhere in it.
	/// </para>
	/// <para>
	/// Now the meter filling starts a WALK. Something is named, it is a few seconds away, and
	/// a keeper who knows what it is can answer it - see <see cref="Give"/>.
	/// </para>
	/// </remarks>
	public static int ApproachRite = -1;

	/// <summary>Seconds until it gets here.</summary>
	public static double ApproachSeconds;

	/// <summary>What this particular walk started with. Relics can lengthen the warning, so a
	/// presentation layer dividing by the constant would read past 1 for a patient keeper.</summary>
	public static double ApproachTotal = kApproachSeconds;

	public static bool Approaching => ApproachRite >= 0;

	/// <summary>How long a keeper has to answer. Long enough to read the line and decide,
	/// short enough that it stays a moment rather than a menu.</summary>
	public const double kApproachSeconds = 9.0;

	/// <summary>The one answer that turns the current visitor away.</summary>
	public static Answer CorrectAnswer => Approaching ? Content.Rites[ApproachRite].Answer : Answer.None;

	/// <summary>What offering it something costs. Priced off production so it stays a real
	/// decision at every tier rather than free by the second hour.</summary>
	public static double OfferCost => (20.0 + Rate * 8.0) * Math.Max(0.1, 1.0 - Wearing(Power.Almsgiving));

	/// <summary>Raised when something starts walking, with the rite whose visitor it is. The
	/// presentation layer names it and starts the clock; nothing here knows that.</summary>
	public static Action<int>? OnApproach;

	/// <summary>Where the draw for the next visitor comes from. Replaceable so the balance
	/// harness can seed it and get the same parish visited the same way twice.</summary>
	public static Random Rng = new Random();

	/// <summary>
	/// 0 to 1. Built by gathering with your own hands, and it drains when you stop.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Not saved, and it belongs HERE rather than with the persistent state it used to be
	/// filed under: it empties in twelve seconds, so it measures whether the keeper is
	/// gathering RIGHT NOW and there is nothing in it a reload could meaningfully restore.
	/// Listing it as persistent only invited someone to wonder why it was not in the save.
	/// </para>
	/// <para>
	/// The reason to touch the screen. Hand gathering was worth a twentieth of a second of
	/// production per click, so there was never a point to it once the first rite was up.
	/// Fervour makes clicking worth something WITHOUT making it mandatory: idle play simply
	/// runs at the base rate, which is what the whole economy is tuned against.
	/// </para>
	/// </remarks>
	public static double Fervour;

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

	/// <summary>
	/// What the worn relics add up to for one power, as a fraction.
	/// </summary>
	/// <remarks>
	/// Summed, not multiplied, and only across WORN relics. Summing keeps three good relics
	/// from compounding into something the balance was never checked against, and it makes the
	/// numbers on the page add up the way a player reading them expects.
	/// </remarks>
	public static double Wearing(Power power)
	{
		double total = 0.0;
		foreach (Relic relic in Worn)
		{
			if (!relic.Exists)
			{
				continue;
			}
			for (int i = 0; i < Relics.PowerCount(relic.Grade); i++)
			{
				if (Relics.PowerAt(relic, i) == power)
				{
					total += Relics.MagnitudeAt(relic, i);
				}
			}
		}
		return total;
	}

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
		double mult = MilestoneMultiplier(rite) * ConsecrationFactor(rite);
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			if (OfferingsTaken[i] && Content.Offerings[i].Target == rite)
			{
				mult *= Content.Offerings[i].Multiplier;
			}
		}
		return mult * GlobalMultiplier;
	}

	/// <summary>
	/// Which rite this keeper has consecrated, or -1.
	/// </summary>
	/// <remarks>
	/// The one decision in a vigil that cannot be taken back and cannot be bought out of. Every
	/// other choice in the game is a purchase - reversible in effect if not in ichor, and made
	/// again next run without consequence - which means a run has no shape to it beyond how far
	/// down the ladder the keeper got. Committing to one rite gives a run an identity: a keeper
	/// who consecrates the Grave Lantern is playing a different game from one who waits and
	/// consecrates the Ossuary Engine, and neither can find out how the other went without
	/// starting again.
	/// </remarks>
	public static int Consecrated = -1;

	/// <summary>What consecration does to a rite's output.</summary>
	/// <remarks>
	/// A bargain, like everything here that survived contact with the harness: the chosen rite
	/// is worth three times as much and every other rite loses a fifth. A free choice of "which
	/// rite gets better" is not a decision, it is a formality with a menu in front of it.
	/// </remarks>
	public const double ConsecratedGain = 3.0;
	public const double ForsakenLoss = 0.80;

	/// <summary>The consecration's effect on one rite. Neutral until a keeper has chosen.</summary>
	public static double ConsecrationFactor(int rite)
	{
		if (Consecrated < 0)
		{
			return 1.0;
		}
		return rite == Consecrated ? ConsecratedGain : ForsakenLoss;
	}

	/// <summary>
	/// Give a vigil its shape. Once, and never undone.
	/// </summary>
	/// <remarks>
	/// Refuses a second attempt rather than replacing the first, and refuses a rite the keeper
	/// does not hold - consecrating something you have never owned would be a way to take the
	/// decision without making it.
	/// </remarks>
	public static bool Consecrate(int rite)
	{
		if (Consecrated >= 0 || rite < 0 || rite >= Content.RiteCount || Owned[rite] <= 0)
		{
			return false;
		}
		Consecrated = rite;
		Revision++;
		Say("You consecrate the " + Content.Rites[rite].Name + ". The rest of the parish feels it.",
			Omen.Good);
		return true;
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

	/// <summary>Each sigil ever taken is worth six percent of everything, forever - whether or
	/// not it is still in hand.</summary>
	public static double SigilMultiplier => 1.0 + 0.06 * SigilsEarned;

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
	public static double DreadMultiplier
		=> 1.0 + (2.6 + BoonFactor(Boon.OldBargain)) * (1.0 + Wearing(Power.Bargain)) * Math.Pow(Dread, 1.4);

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

	/// <summary>What fervour actually drains at, after a keeper has communed for a steadier
	/// hand.</summary>
	public static double FervourDrain
		=> kFervourDrain * Math.Max(0.05, 1.0 - BoonFactor(Boon.SteadyHand) - Wearing(Power.Steadiness));

	/// <summary>What one hand gather adds to fervour. Roughly three seconds of steady
	/// clicking to fill it, and about twelve seconds of not clicking to lose it.</summary>
	public const double kFervourPerGather = 0.075;

	/// <summary>What fervour is worth at the top: a little over half again.</summary>
	public static double FervourMultiplier => 1.0 + Fervour * 0.6;

	/// <summary>
	/// How fast dread lets go, as a fraction of what is there, per second.
	/// </summary>
	/// <remarks>
	/// <para>
	/// An idle game must not punish idling. Dread that only ever climbs turns "leave it
	/// running" - the thing the whole genre is built on - into the losing move, and makes a
	/// button you have to come back and press the price of playing at all. With decay, a
	/// parish left alone settles at whatever level its holdings sustain and stays there. A
	/// visitation becomes something you walk INTO by reaching for the multiplier, never
	/// something that happens because you looked away.
	/// </para>
	/// <para>
	/// It relaxes PROPORTIONALLY now rather than subtracting a flat 0.004/s, and that one
	/// change is what gives the early game its central mechanic back. A flat floor meant any
	/// parish whose pressure sat under it could not move the meter AT ALL: a keeper with
	/// nothing but grave lanterns pushed 0.0007/s against a 0.004/s drain, so dread read zero,
	/// the bargain paid x1, and the first visitation of a clean run did not arrive for eleven
	/// minutes. Relaxation has no floor, so every holding shows on the meter from the first
	/// one, and the parish settles at <see cref="DreadEquilibrium"/> instead of at zero.
	/// </para>
	/// </remarks>
	public const double DreadRelax = 0.02;

	/// <summary>
	/// What the echoes are carrying, 0 to 1 across the whole line.
	/// </summary>
	/// <remarks>
	/// Averaged over <see cref="MaxEchoes"/> rather than over however many echoes exist, so
	/// handing everything to a single keeper is not a way to make the burden read as full while
	/// the line is mostly empty - the debt is the line's, not one ghost's.
	/// </remarks>
	public static double BurdenTotal
	{
		get
		{
			double total = 0.0;
			foreach (Echo echo in Echoes)
			{
				total += echo.Burden;
			}
			return Math.Clamp(total / MaxEchoes, 0.0, 1.0);
		}
	}

	/// <summary>What a fully burdened line adds to the pressure on the meter, per second. Set
	/// so that leaning on the dead roughly halves the peace between visitations rather than
	/// nudging it: a cost the keeper cannot feel is not a cost they will weigh.</summary>
	public const double kBurdenPressure = 0.05;

	/// <summary>
	/// How fast an echo puts down what it is carrying, per second.
	/// </summary>
	/// <remarks>
	/// Slow enough that a shunt is a decision you live with, fast enough that a keeper who
	/// stops leaning gets their meter back inside a session: a fully loaded line clears in a
	/// bit under two hours. It was four and a half at first, which made one press of one button
	/// a penalty carried for the rest of the day - a cost that outlives the situation that
	/// justified it stops reading as a trade and starts reading as a mistake.
	/// </remarks>
	public const double kBurdenEase = 0.00015;

	/// <summary>Most one shunt can move. The same quarter the congregation verb pushes onto
	/// another player, because it is the same act - only the recipient has changed.</summary>
	public const double kShuntShare = 0.25;

	/// <summary>What an echo can hold before it will not take any more.</summary>
	public const double kEchoCapacity = 1.0;

	/// <summary>True when this echo has room for more.</summary>
	public static bool CanShunt(int index)
		=> index >= 0 && index < Echoes.Count && Dread > 0.02 && ShuntCooldown <= 0.0
			&& Echoes[index].Burden < kEchoCapacity - kShuntShare * 0.5;

	/// <summary>Seconds before the line will take anything else. Without it a keeper can hold
	/// the meter at any level they like by shunting the trickle that easing frees up every
	/// frame - simulated, that was seventy-two thousand shunts in ninety minutes, which is not
	/// a decision anybody is making.</summary>
	public static double ShuntCooldown;

	public const double kShuntInterval = 20.0;

	/// <summary>
	/// Push what you are carrying onto one of the keepers you used to be.
	/// </summary>
	/// <remarks>
	/// Returns what actually moved, which is never more than the echo has room for - so a
	/// keeper cannot shed a quarter into a ghost that could only take a tenth and quietly lose
	/// the rest. The same care <see cref="ShedDread"/> takes for the same reason.
	/// </remarks>
	public static double ShuntToEcho(int index)
	{
		if (!CanShunt(index))
		{
			return 0.0;
		}
		Echo echo = Echoes[index];
		double room = kEchoCapacity - echo.Burden;
		double moved = Math.Min(Math.Min(Dread, kShuntShare), room);
		if (moved <= 0.0)
		{
			return 0.0;
		}
		Dread -= moved;
		echo.Burden += moved;
		Echoes[index] = echo;
		ShuntCooldown = kShuntInterval;
		Revision++;
		Say(echo.Name + " takes it from you. They do not seem to mind, which is worse.", Omen.Dread);
		return moved;
	}

	/// <summary>
	/// Remember the keeper this run was, so a later one can lean on them.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The line is capped, and once it is full the newcomer takes the LIGHTEST-burdened
	/// keeper's place and inherits what they were carrying. Both halves of that matter.
	/// </para>
	/// <para>
	/// Inheriting the burden is what stops communion laundering the debt. Drop the heaviest
	/// echo instead and a keeper clears a fully loaded line by communing four times, which
	/// turns the one real cost of shunting into a formality - and communion is a thing players
	/// already do constantly for sigils, so it would not even be a detour.
	/// </para>
	/// <para>
	/// Taking their PLACE rather than being appended is what stops the line going stale. The
	/// first version simply dropped the lightest, which meant a fresh echo carrying nothing was
	/// always the one evicted: past four communions the panel showed the same four names
	/// forever and no keeper ever saw themselves join the line they were adding to.
	/// </para>
	/// </remarks>
	private static void RecordEcho()
	{
		Echo fresh = new Echo { Name = KeeperName, Burden = 0.0 };
		if (Echoes.Count < MaxEchoes)
		{
			Echoes.Add(fresh);
			return;
		}

		int lightest = 0;
		for (int i = 1; i < Echoes.Count; i++)
		{
			if (Echoes[i].Burden < Echoes[lightest].Burden)
			{
				lightest = i;
			}
		}
		// Removed and appended rather than overwritten in place, so the list stays in the order
		// the keepers arrived. Overwriting put every newcomer into the same slot whenever the
		// burdens were level - which they are once a line is full - so one name cycled and the
		// other three were fossils.
		fresh.Burden = Echoes[lightest].Burden;
		Echoes.RemoveAt(lightest);
		Echoes.Add(fresh);
	}

	/// <summary>The pressure the parish puts on the meter, before the ward holds any of it
	/// back. Owning more raises this; it is the reason a big parish is a dangerous one.</summary>
	public static double DreadPressure
	{
		get
		{
			double total = 0.0;
			for (int i = 0; i < Content.RiteCount; i++)
			{
				total += Owned[i] * Content.Rites[i].DreadRate * RiteDreadScale(i);
			}
			// A late run would otherwise pin the meter within seconds. Owning more of a tier
			// should raise dread; owning ALL the tiers should not make the game unplayable,
			// so the climb is compressed rather than summed flat.
			//
			// Capped as well as compressed, because the square root still grows without bound
			// and the interval between visitations is its reciprocal: an unbounded pressure is
			// a late game that strobes, where the dark arrives every few seconds forever and no
			// aftermath can be short enough to fit between two of them. The cap gives the
			// deepest parish a floor on its rhythm - about a quarter of a minute - and the
			// danger stops escalating past the point where escalating it only means flicker.
			// The burden is added AFTER the cap, not folded in before it, and that is the whole
			// bite of a shunt. Inside the cap it would vanish for any parish already at the
			// ceiling - which is precisely the keeper rich enough to be leaning on the dead.
			//
			// It was tried the other way first, as a drag on relaxation instead of a push on
			// pressure. That reads better and does almost nothing: halving relaxation doubles
			// the equilibrium and the time between visitations together, so a fully burdened
			// line moved the window from 21 seconds to 19. Pressure shortens the window
			// directly, which is what "you will answer for this" has to mean here.
			return Math.Min(Math.Sqrt(total) * 0.022, kMaxPressure) + BurdenTotal * kBurdenPressure;
		}
	}

	/// <summary>What the meter climbs at right now: pressure, less what is already letting
	/// go. Wards do not touch this - they catch what happens when it arrives at the top.</summary>
	public static double DreadRate => DreadPressure - DreadRelax * Dread;

	/// <summary>Where the meter settles if nothing is stoked and nothing is bought. Above 1
	/// means this parish will keep walking itself into visitations on its own.</summary>
	public static double DreadEquilibrium => DreadPressure / DreadRelax;

	/// <summary>
	/// Seconds until the meter reaches the top from where it is, or infinity if this parish
	/// cannot get there by itself.
	/// </summary>
	/// <remarks>
	/// The closed form of the relaxation above, and the number the rest of the economy is
	/// quoted against: a ward is priced off it and an aftermath is measured in it, so both
	/// stay a fixed SHARE of what a keeper earns between visitations however deep the parish
	/// gets. It is also worth showing the player - the whole game is a clock they are choosing
	/// to stand next to, and they should be able to read it.
	/// </remarks>
	public static double SecondsToVisitation
	{
		get
		{
			double equilibrium = DreadEquilibrium;
			if (equilibrium <= 1.0 || Dread >= 1.0)
			{
				return equilibrium <= 1.0 ? double.PositiveInfinity : 0.0;
			}
			return Math.Log((equilibrium - Dread) / (equilibrium - 1.0)) / DreadRelax;
		}
	}

	/// <summary>The same clock measured from a standing start, which is what a ward and an
	/// aftermath are both priced against. Clamped so a parish that will never get there on its
	/// own does not quote an infinite price for insurance it does not need.</summary>
	private static double VisitationWindow
	{
		get
		{
			double equilibrium = DreadEquilibrium;
			if (equilibrium <= 1.0)
			{
				return kMaxWindow;
			}
			return Math.Clamp(Math.Log(equilibrium / (equilibrium - 1.0)) / DreadRelax, kMinWindow, kMaxWindow);
		}
	}

	/// <summary>The most pressure any parish can put on the meter. Sets the floor on the
	/// window - see <see cref="DreadPressure"/> for why there has to be one.</summary>
	private const double kMaxPressure = 0.075;

	/// <summary>Bounds on the window a ward and an aftermath are priced against. The lower one
	/// sits just under what <see cref="kMaxPressure"/> allows, so it is a guard rather than a
	/// rule: if it ever binds, insurance is being sold for a window it does not cover.</summary>
	private const double kMinWindow = 14.0;
	private const double kMaxWindow = 240.0;

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
			// The parish slice is taken off Rate, which ALREADY carries the global multiplier
			// through RiteMultiplier - so only the bare hand value is multiplied by it. It used
			// to be `(hand + Rate * 0.05) * GlobalMultiplier`, which applied the multiplier to
			// the slice twice and made hand gathering scale as the SQUARE of every bonus in the
			// game. Compounding, so it grew worse exactly as a run went on.
			//
			// A relic's Hand power multiplies the WHOLE figure, though, because that is what its
			// label promises. Applied to the bare term only it reached all of the figure early
			// and none of it later - measured, a relic claiming five percent by hand moved the
			// number by nothing at all once a parish was three tiers deep, since the slice off
			// production had swamped the term being boosted. A power that quietly stops working
			// as you progress is worse than one that was never offered.
			return (hand * GlobalMultiplier + Rate * 0.05) * (1.0 + Wearing(Power.Hand));
		}
	}

	/// <summary>
	/// What raising a ward costs: a fixed share of everything the parish earns in the window
	/// one ward covers.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Priced off the WINDOW, not off the rate. It used to be <c>RawRate * 12</c> - linear in
	/// production - while the window itself shrinks as the parish grows, so insurance quietly
	/// ate a larger share of income at every tier: 6% of the window at one of each rite, 43%
	/// at twenty, 91% at eighty, and past about a hundred a keeper could no longer afford to
	/// stay insured at all. That is the same "progress makes the game worse" failure the ward
	/// was rewritten to remove, arriving late instead of early.
	/// </para>
	/// <para>
	/// Quoting it as a share of the window fixes it for every parish that will ever exist,
	/// including ones with rites nobody has written yet: a ward always costs a quarter of what
	/// you make while it stands, so the decision to insure reads identically at hour one and
	/// hour ten.
	/// </para>
	/// </remarks>
	public static double WardCost
		=> (40.0 + RawRate * VisitationWindow * kWardShare) * Math.Max(0.1, 1.0 - Wearing(Power.Warding));

	/// <summary>What fraction of a window's income a ward costs. Set just above what eating
	/// the aftermath costs instead (<see cref="kAftermathShare"/> of the window at half pace),
	/// so insurance is the dearer option in raw ichor and buys a better place on the meter -
	/// which is what makes it a decision rather than an obvious purchase.</summary>
	private const double kWardShare = 0.25;

	/// <summary>
	/// Sigils a communion would pay right now.
	/// </summary>
	/// <remarks>
	/// A fourth root, not a square root, because the thing it is measuring is exponential. At
	/// <c>8 * sqrt(run / 1e7)</c> an ordinary half-hour offered 105 sigils and two hours offered
	/// twenty-four thousand, against a game with 108 sigils of things to buy - so the first
	/// communion bought everything and prestige collapsed into a flat percentage on the second
	/// run. Prestige has to grow far more slowly than the economy it is drawn from or it stops
	/// being a currency at all.
	/// </remarks>
	public static int SigilsOnOffer
		=> (int)Math.Floor(kSigilScale * Math.Pow(Math.Max(0.0, RunIchor) / kSigilBase, 0.25));

	/// <summary>Run ichor needed for <paramref name="sigils"/> to be on offer - the exact
	/// inverse of the payout, so a panel promising a target cannot promise one the button
	/// will not honour.</summary>
	public static double RunIchorForSigils(int sigils)
		=> Math.Pow(Math.Max(0, sigils) / kSigilScale, 4.0) * kSigilBase;

	private const double kSigilScale = 3.0;
	private const double kSigilBase = 2e8;

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
	/// <summary>
	/// How much of this rite's dread still lands, from offerings taken. Below 1 is quieter.
	/// </summary>
	/// <remarks>
	/// Floored at a third and never at zero. The bargain of the game is that the deep rites pay
	/// better and pull the dark in faster; an offering that let a keeper buy that off entirely
	/// would not make the parish safer, it would make it pointless - there would be nothing left
	/// to weigh against anything. Quieter is a decision. Silent is the end of the decision.
	/// </remarks>
	public static double RiteDreadScale(int rite)
	{
		double scale = 1.0;
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			if (OfferingsTaken[i] && Content.Offerings[i].Target == rite)
			{
				scale *= Content.Offerings[i].DreadScale;
			}
		}
		return Math.Max(0.33, scale);
	}

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

	/// <summary>Raised when the parish gives something up. The presentation layer names it and
	/// throws it on screen; nothing here knows that.</summary>
	public static Action<Relic>? OnFound;

	public static void Gather()
	{
		double gain = HandGain;
		Ichor += gain;
		RunIchor += gain;
		LifetimeIchor += gain;
		HandGathers++;
		Fervour = Math.Min(1.0, Fervour + kFervourPerGather);

		// You are gathering with your hands, in a parish full of buried things, and the deeper
		// in the dark you are the better what you turn up. This is what stops hand-gathering
		// being the same chore at hour ten as at hour one: measured, a click is worth a flat
		// twentieth of a second of production forever, whatever the parish is doing.
		Relic found = Relics.Dig(Rng, Dread, ++RelicSeed);
		if (!found.Exists)
		{
			return;
		}
		RelicsFound++;
		if (Satchel.Count >= Relics.Satchel)
		{
			// Full. The oldest LEAVINGS go first, and only if the newcomer is better than it -
			// so a satchel of good things is never quietly emptied by a run of rubbish.
			int worst = -1;
			for (int i = 0; i < Satchel.Count; i++)
			{
				if (worst < 0 || Satchel[i].Grade < Satchel[worst].Grade)
				{
					worst = i;
				}
			}
			if (worst < 0 || Satchel[worst].Grade > found.Grade)
			{
				return;
			}
			Satchel.RemoveAt(worst);
		}
		Satchel.Add(found);
		Revision++;
		// Only the ones worth remarking on. Announcing every find put 45 lines into a ten-minute
		// session against 13 before, which is the parish going back to being a notification tray
		// - the exact thing its voice was written to stop being. Leavings and keepsakes land in
		// the satchel quietly and the panel is where you notice them.
		if (found.Grade >= Grade.Anointed)
		{
			Say("You turn something up: " + Relics.NameOf(found) + ".", Omen.Good);
		}
		OnFound?.Invoke(found);
	}

	/// <summary>Put a carried relic on, swapping out whatever was in that slot.</summary>
	public static bool Wear(int satchelIndex, int slot)
	{
		if (satchelIndex < 0 || satchelIndex >= Satchel.Count || slot < 0 || slot >= Relics.Slots)
		{
			return false;
		}
		Relic taking = Satchel[satchelIndex];
		Satchel.RemoveAt(satchelIndex);
		if (Worn[slot].Exists)
		{
			Satchel.Add(Worn[slot]);
		}
		Worn[slot] = taking;
		Revision++;
		return true;
	}

	/// <summary>
	/// How good a relic is, as one number.
	/// </summary>
	/// <remarks>
	/// Grade dominates, because a better grade carries MORE POWERS rather than bigger ones and
	/// three powers beat one whatever the rolls were. Magnitude only settles ties within a
	/// grade. The grade term is scaled far past any possible sum of magnitudes so the ordering
	/// can never invert: a Hollowed thing with poor rolls still outranks a lucky Keepsake.
	/// </remarks>
	public static double RelicWorth(Relic relic)
	{
		if (!relic.Exists)
		{
			return -1.0;
		}
		double magnitude = 0.0;
		for (int i = 0; i < Relics.PowerCount(relic.Grade); i++)
		{
			magnitude += Relics.MagnitudeAt(relic, i);
		}
		return (int)relic.Grade * 1000.0 + magnitude;
	}

	/// <summary>
	/// Wear the best three relics the keeper has, from what is worn and what is carried.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Every find used to mean opening the satchel and comparing rows by hand, and since a
	/// better grade is simply better there was never a judgement in it - just bookkeeping the
	/// player was made to do because the game would not. A button that does the obvious thing is
	/// not a loss of decision when there was no decision to lose.
	/// </para>
	/// <para>
	/// Works on worn AND carried together, so it is idempotent: pressing it twice does nothing
	/// the second time, and pressing it when the loadout is already best does nothing at all.
	/// It can only ever improve the hand, which is what makes it safe to press without reading
	/// anything first.
	/// </para>
	/// </remarks>
	/// <summary>
	/// Would <see cref="WearBest"/> actually change anything?
	/// </summary>
	/// <remarks>
	/// Asked so the button can be dark when it would do nothing. A control that is always
	/// available and usually pointless trains a keeper to press it out of habit and stop reading
	/// it; one that lights up when it has something to say is worth glancing at.
	/// </remarks>
	public static bool WouldWearBestChange()
	{
		double worstWorn = double.MaxValue;
		int wornCount = 0;
		foreach (Relic worn in Worn)
		{
			if (worn.Exists)
			{
				wornCount++;
				worstWorn = Math.Min(worstWorn, RelicWorth(worn));
			}
		}
		if (wornCount < Relics.Slots)
		{
			// A free hand and anything at all to put in it.
			return Satchel.Count > 0;
		}
		foreach (Relic carried in Satchel)
		{
			if (RelicWorth(carried) > worstWorn)
			{
				return true;
			}
		}
		return false;
	}

	/// <returns>How many slots changed.</returns>
	public static int WearBest()
	{
		// Worn first in the pool, and ties broken in favour of staying worn. Without that, two
		// relics of equal worth swap places every time the button is pressed - the hand is no
		// better, the button never goes dark, and pressing it again keeps moving things. Churn
		// that gains nothing reads as the button not working.
		List<Relic> pool = new List<Relic>();
		int alreadyWorn = 0;
		foreach (Relic worn in Worn)
		{
			if (worn.Exists)
			{
				pool.Add(worn);
				alreadyWorn++;
			}
		}
		pool.AddRange(Satchel);
		if (pool.Count == 0)
		{
			return 0;
		}

		int[] order = new int[pool.Count];
		for (int i = 0; i < order.Length; i++)
		{
			order[i] = i;
		}
		Array.Sort(order, (a, b) =>
		{
			int byWorth = RelicWorth(pool[b]).CompareTo(RelicWorth(pool[a]));
			if (byWorth != 0)
			{
				return byWorth;
			}
			// Equal worth: whatever was already worn keeps its place.
			bool aWorn = a < alreadyWorn;
			bool bWorn = b < alreadyWorn;
			if (aWorn != bWorn)
			{
				return aWorn ? -1 : 1;
			}
			return a.CompareTo(b);
		});
		List<Relic> sorted = new List<Relic>(pool.Count);
		foreach (int index in order)
		{
			sorted.Add(pool[index]);
		}
		pool = sorted;

		Relic[] before = (Relic[])Worn.Clone();

		// Which relics end up worn is settled by the sort; WHERE each one sits is settled by
		// where it already sat. Assigning the sorted list straight down the slots reordered the
		// hand every time - a low relic in slot 0 and a high one in slot 1 would swap, changing
		// both slots to reach an identical hand. Slot order means nothing (Wearing sums across
		// all three), so that was churn, and it made the button report work it had not done.
		int keep = Math.Min(Relics.Slots, pool.Count);
		Relic[] hands = new Relic[Relics.Slots];
		bool[] placed = new bool[keep];
		bool[] kept = new bool[Relics.Slots];

		// Anything still in the best three stays in the slot it is already in.
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			if (!before[slot].Exists)
			{
				continue;
			}
			for (int i = 0; i < keep; i++)
			{
				if (!placed[i] && pool[i].Seed == before[slot].Seed && pool[i].Grade == before[slot].Grade)
				{
					placed[i] = true;
					hands[slot] = pool[i];
					kept[slot] = true;
					break;
				}
			}
		}

		// Whatever is left of the best three fills the slots nothing kept. A slot whose relic
		// dropped out is simply not kept, and so is written over here - it must NOT be left
		// holding its old relic, or that relic ends up both worn and back in the satchel.
		int cursor = 0;
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			if (kept[slot])
			{
				continue;
			}
			while (cursor < keep && placed[cursor])
			{
				cursor++;
			}
			if (cursor < keep)
			{
				placed[cursor] = true;
				hands[slot] = pool[cursor];
			}
		}

		Satchel.Clear();
		for (int slot = 0; slot < Relics.Slots; slot++)
		{
			Worn[slot] = hands[slot];
		}
		for (int i = keep; i < pool.Count; i++)
		{
			Satchel.Add(pool[i]);
		}

		int moved = 0;
		for (int i = 0; i < Relics.Slots; i++)
		{
			// Seed and grade ARE the relic - Exists is derived from the seed - so these two
			// fields settle whether the slot changed.
			if (before[i].Seed != Worn[i].Seed || before[i].Grade != Worn[i].Grade)
			{
				moved++;
			}
		}
		if (moved > 0)
		{
			Revision++;
		}
		return moved;
	}

	/// <summary>Take a relic off and put it back in the satchel.</summary>
	public static bool Remove(int slot)
	{
		if (slot < 0 || slot >= Relics.Slots || !Worn[slot].Exists || Satchel.Count >= Relics.Satchel)
		{
			return false;
		}
		Satchel.Add(Worn[slot]);
		Worn[slot] = default;
		Revision++;
		return true;
	}

	/// <summary>
	/// Hand a carried relic over, taking it out of the satchel as it goes.
	/// </summary>
	/// <remarks>
	/// Removed HERE and only sent if the removal succeeded, exactly as a tithe deducts before
	/// it sends. This process is the only one that knows what is in this keeper's satchel, so
	/// it is the only one that can spend it - a message sent first and deducted afterwards is
	/// how one relic becomes two.
	/// </remarks>
	public static bool GiveRelic(int satchelIndex, out Relic given)
	{
		given = default;
		if (satchelIndex < 0 || satchelIndex >= Satchel.Count)
		{
			return false;
		}
		given = Satchel[satchelIndex];
		Satchel.RemoveAt(satchelIndex);
		Revision++;
		return true;
	}

	/// <summary>
	/// A relic arriving from another keeper.
	/// </summary>
	/// <remarks>
	/// <para>
	/// It always lands. A full satchel drops its worst carried relic to make room, because the
	/// alternative is refusing a relic that has ALREADY left the sender's hands - and a trade
	/// that destroys the thing being traded is worse than one that costs the receiver their
	/// cheapest keepsake. Worn relics are never displaced; nothing a keeper is relying on
	/// disappears because somebody was generous.
	/// </para>
	/// <para>
	/// The grade is clamped rather than trusted. A relic is two numbers on the wire, and while
	/// nothing can be forged into an item that does not exist - every seed is a valid relic -
	/// a grade outside the ladder would index the name tables and the art shader with a number
	/// neither was written for.
	/// </para>
	/// </remarks>
	public static void ReceiveRelic(int seed, int grade, string from)
	{
		if (seed == 0)
		{
			return;
		}
		Relic arriving = new Relic
		{
			Seed = seed,
			Grade = (Grade)Math.Clamp(grade, 0, (int)Grade.Hollowed),
		};

		MakeRoom();
		Satchel.Add(arriving);
		Revision++;
		Say(from + " puts " + Relics.NameOf(arriving) + " into your hands.", Omen.Good);
	}

	/// <summary>Put a relic back after an offer failed to leave. Silent, because nothing
	/// happened as far as the keeper is concerned - routing this through ReceiveRelic produced
	/// a line about somebody handing you your own relic, which is a lie about an event that
	/// did not occur.</summary>
	public static void RestoreRelic(Relic relic)
	{
		if (!relic.Exists)
		{
			return;
		}
		MakeRoom();
		Satchel.Add(relic);
		Revision++;
	}

	/// <summary>Drop the worst CARRIED relic if the satchel is full. Worn relics are never
	/// touched: nothing a keeper is relying on disappears because something arrived.</summary>
	private static void MakeRoom()
	{
		if (Satchel.Count < Relics.Satchel)
		{
			return;
		}
		int worst = 0;
		for (int i = 1; i < Satchel.Count; i++)
		{
			if (Satchel[i].Grade < Satchel[worst].Grade)
			{
				worst = i;
			}
		}
		Satchel.RemoveAt(worst);
	}

	/// <summary>What rendering the relic at this index would pay.</summary>
	public static double RenderValue(int satchelIndex)
	{
		if (satchelIndex < 0 || satchelIndex >= Satchel.Count)
		{
			return 0.0;
		}
		// A floor off the hand, so an early keeper with no parish still gets something back
		// rather than being told their find was worth nothing.
		return HandGain * 1.5 + Rate * Relics.RenderSeconds(Satchel[satchelIndex].Grade);
	}

	/// <summary>
	/// Render a carried relic down for ichor.
	/// </summary>
	/// <remarks>
	/// Worn relics cannot be rendered without being taken off first, so nothing a keeper is
	/// relying on goes into the pot on a misclick.
	/// </remarks>
	public static bool Render(int satchelIndex)
	{
		if (satchelIndex < 0 || satchelIndex >= Satchel.Count)
		{
			return false;
		}
		Relic rendered = Satchel[satchelIndex];
		double paid = RenderValue(satchelIndex);
		Satchel.RemoveAt(satchelIndex);
		// Said, because until now a rendered relic simply stopped existing: no line, no figure,
		// nothing to tell a keeper whether they had rendered it, dropped it or hit a bug. It
		// goes through the parish's voice rather than a popup so it also lands in the transcript,
		// which is where somebody goes when they are not sure what just happened.
		Say("Rendered " + Relics.NameOf(rendered) + " for " + Numbers.Short(paid) + " ichor.", Omen.Plain);
		Ichor += paid;
		RunIchor += paid;
		LifetimeIchor += paid;
		Revision++;
		if (rendered.Grade >= Grade.Hallowed)
		{
			// Only worth remarking on when a keeper melts something good - the feed is a shared
			// budget and rendering junk is a thing they will do dozens of times.
			Say("You render down " + Relics.NameOf(rendered) + ". It goes quietly.", Omen.Plain);
		}
		return true;
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
	/// <summary>
	/// What the dark is offering for one more step toward it.
	/// </summary>
	/// <remarks>
	/// <para>
	/// A property rather than an expression at the call site, because the HUD quotes this
	/// number on the button and the two copies had already drifted apart - both of them
	/// wrong in the same way, and neither able to be corrected without the other.
	/// </para>
	/// <para>
	/// It PAYS, because stoking used to add risk and nothing else, which made it a button with
	/// no argument for pressing it. But it pays a fraction of one visitation window rather than
	/// fifteen flat seconds of production times the global multiplier a second time, and it
	/// pays LESS the closer the meter already is to the top. That shape is the whole point:
	/// the first step into the dark is bought cheaply, the last one is almost a gift you give
	/// it, and so the reason to climb has to be the multiplier waiting at the top rather than
	/// the handout on the way. Pressed at a human two-per-second against the old formula, this
	/// button was worth five million times a clean run over half an hour - not a strategy, an
	/// exit from the game.
	/// </para>
	/// </remarks>
	public static double StokeOffer => (Rate + HandGain) * kStokeSeconds * (1.0 - Dread);

	/// <summary>
	/// Seconds of income one stoke offers at an empty meter.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Chosen so a full climb from nothing pays roughly what one visitation costs - stoking is
	/// close to free in ichor and expensive in exposure, which is the trade it is meant to be.
	/// </para>
	/// <para>
	/// Measured against <see cref="Rate"/> PLUS <see cref="HandGain"/>, and the second term is
	/// there because dropping it broke the early game. A keeper with one lantern has a rate of
	/// 0.1/s, so a rate-only offer paid 0.4 ichor against a 16-ichor lantern while still
	/// costing eighteen points of the meter: a button that visibly did nothing, at exactly the
	/// moment a player is deciding whether it is worth pressing. Their hands are most of their
	/// income for the first few minutes, so their hands have to count. This is not the old
	/// double-multiplier bug returning - HandGain already carries the global multiplier exactly
	/// once, and nothing is re-applied here.
	/// </para>
	/// </remarks>
	private const double kStokeSeconds = 4.0;

	/// <summary>Seconds before the dark will take another step. Not saved: it outlives nothing,
	/// and a reload to skip five seconds is more trouble than the five seconds.</summary>
	public static double StokeCooldown;

	/// <summary>How long between stokes, after any communion has shortened it.</summary>
	public static double StokeInterval => kStokeInterval * (1.0 - BoonFactor(Boon.QuickKindling));

	private const double kStokeInterval = 5.0;

	/// <summary>Seconds between stoke lines, however often the button is pressed.</summary>
	private const double kStokeLineGap = 25.0;

	/// <summary>When the stoke line was last said, in played seconds. Measured against played
	/// time rather than wall time so it means the same thing in a test as in a session.</summary>
	private static double s_lastStokeLine;

	public static bool CanStoke => Dread < 0.999 && StokeCooldown <= 0.0;

	public static bool Stoke()
	{
		if (!CanStoke)
		{
			return false;
		}
		double offered = StokeOffer;
		Dread = Math.Min(1.0, Dread + 0.18);
		StokeCooldown = StokeInterval;
		Ichor += offered;
		RunIchor += offered;
		LifetimeIchor += offered;
		// Not every press. A keeper riding the meter stokes nine times a minute, and nine
		// identical sentences a minute is not atmosphere, it is a log - measured, the feed ran
		// at 30 lines a minute against the six-line, nine-second window it has to show them in,
		// so lines were being pushed off before they could be read. The meter moves and the
		// purse changes on every stoke; the SENTENCE is for the moments it is worth saying.
		if (s_lastStokeLine <= 0.0 || PlayedSeconds - s_lastStokeLine >= kStokeLineGap)
		{
			s_lastStokeLine = PlayedSeconds;
			Say("You lean closer. It gives you " + Numbers.Short(offered) + " and remembers.", Omen.Dread);
		}
		return true;
	}

	// ── Boons ────────────────────────────────────────────────────────────────────────

	public static int BoonLevel(Boon boon) => Boons[(int)boon];

	/// <summary>Levels held times what one level is worth. Every formula a boon touches reads
	/// it through here, so the table is the only place a boon's magnitude is written down.</summary>
	public static double BoonFactor(Boon boon) => Boons[(int)boon] * Content.Boons[(int)boon].Step;

	/// <summary>What the next level of a boon costs, in sigils. Zero once it is maxed.</summary>
	public static int BoonCost(int index)
	{
		BoonDef def = Content.Boons[index];
		int level = Boons[index];
		return level >= def.MaxLevel ? 0 : (int)Math.Ceiling(def.BaseCost * Math.Pow(def.Growth, level));
	}

	public static bool BuyBoon(int index)
	{
		int cost = BoonCost(index);
		if (cost <= 0 || Sigils < cost)
		{
			return false;
		}
		Sigils -= cost;
		Boons[index]++;
		Revision++;
		Say(Content.Boons[index].Name + " deepens. Something of the last vigil stays with you.", Omen.Good);
		OnSpent?.Invoke(-1);
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
		SigilsEarned += payout;
		Communions++;
		RecordEcho();
		Ichor = 0.0;
		RunIchor = 0.0;
		Dread = 0.0;
		// The consecration is what gave THIS run its shape, so it goes with the run. Carrying it
		// across a communion would turn a decision into a permanent upgrade, which the boons
		// already are - and would mean a keeper made the choice once, ten runs ago, and never
		// again.
		Consecrated = -1;
		Wards = 0;
		AftermathSeconds = 0.0;
		StokeCooldown = 0.0;
		ApproachRite = -1;
		ApproachSeconds = 0.0;
		ShuntCooldown = 0.0;
		s_murmurBand = -1;
		s_quiet = 0.0;
		s_beckons = 0;
		s_lastAmbient = -1;
		s_lastStokeLine = 0.0;
		// Both call sites - a fresh vigil AND a communion. Communion clears the offerings
		// taken, so every one of them becomes available again at once; without re-priming, a
		// keeper would be told about thirty offerings in a single frame.
		s_offeringsNoticed = new bool[Content.Offerings.Length];
		s_offeringsPrimed = false;
		s_murmursSaid = new int[Content.Murmurs.Length];
		s_lastSpoken = "";
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
		if (StokeCooldown > 0.0)
		{
			StokeCooldown = Math.Max(0.0, StokeCooldown - deltaSeconds);
		}
		if (ShuntCooldown > 0.0)
		{
			ShuntCooldown = Math.Max(0.0, ShuntCooldown - deltaSeconds);
		}
		// Fervour drains steadily, so it is a reward for playing NOW rather than a level you
		// grind once and keep.
		Fervour = Math.Max(0.0, Fervour - FervourDrain * deltaSeconds);

		// The dead put things down eventually. Applied offline as well as on, because the debt
		// is time passing rather than attention paid - and a keeper who walks away for a night
		// should come back to a line that has eased rather than to the same weight exactly.
		for (int i = 0; i < Echoes.Count; i++)
		{
			Echo echo = Echoes[i];
			if (echo.Burden <= 0.0)
			{
				continue;
			}
			echo.Burden = Math.Max(0.0, echo.Burden - kBurdenEase * deltaSeconds);
			Echoes[i] = echo;
		}

		double efficiency = offline ? OfflineEfficiency : 1.0;
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

		// Captured BEFORE the relaxation below, and that ordering is the difference between the
		// encounter being reachable and being unreachable. Relaxation is proportional to how
		// much dread is there, so at the very top a small parish relaxes FASTER than it pulls -
		// a keeper who stoked all the way to 1.0 had it pulled back to 0.999 in the same tick,
		// before the brink was ever tested. The effect was that no parish under the equilibrium
		// threshold could reach a visitation at all, however deliberately its keeper walked
		// toward one, which quietly gated the whole encounter behind an hour of growth.
		bool reachedTheBrink = Dread >= 1.0;

		// Pressure in, relaxation out, so a parish tends toward an equilibrium rather than a
		// cliff - and one whose equilibrium sits under 1.0 is never visited unprovoked.
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

		if (!offline)
		{
			Murmur(deltaSeconds);
			NoticeOfferings();
		}

		// Offline never starts one: a keeper cannot answer a door they were not behind, and
		// the one promise offline progress makes is that it cannot cost you anything you were
		// not there to defend.
		if (!offline)
		{
			if (Approaching)
			{
				ApproachSeconds -= deltaSeconds;
				if (ApproachSeconds <= 0.0)
				{
					// Nobody answered. Resolve exactly as the game always did - a ward, or the
					// aftermath - because an idle game must not punish the player for idling.
					// The encounter is an OPPORTUNITY for the attentive keeper to do better
					// than the default, never a penalty on the absent one.
					Resolve(Answer.None);
				}
			}
			else if (reachedTheBrink || Dread >= 1.0)
			{
				BeginApproach();
			}
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

	/// <summary>Something starts walking. The meter stays pinned while it does, so the keeper
	/// answers at the top of the bargain rather than watching it drain to safety.</summary>
	private static void BeginApproach()
	{
		int rite = DrawVisitor();
		if (rite < 0)
		{
			// Nothing owned, so nothing to come for. Let the meter sit just under the brink
			// rather than inventing a visitor for an empty parish.
			Dread = 0.85;
			return;
		}
		VisitorsMet[rite] = true;
		ApproachRite = rite;
		ApproachSeconds = kApproachSeconds * (1.0 + Wearing(Power.Patience));
		ApproachTotal = ApproachSeconds;
		Revision++;
		Say(Content.Rites[rite].Approach, Omen.Dread);
		OnApproach?.Invoke(rite);
	}

	/// <summary>
	/// Choose whose visitor arrives, weighted by how much dread that rite is pulling in.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Drawn from the PRESSURE, which is the same quantity that decides how often anything
	/// comes at all - so the thing that arrives is drawn from whatever called it. A parish
	/// leaning on its chapels meets what chapels attract; broadening the parish broadens who
	/// walks in.
	/// </para>
	/// <para>
	/// It used to be simply <c>DeepestRite()</c>, and simulating two hours of play showed what
	/// that cost: five of the eight visitors NEVER appeared, and 91% of encounters were the two
	/// that want a ward. An encounter with one answer is a keypress, not a decision - the whole
	/// mechanic collapsed within minutes of owning a deep rite. Weighting fixes it without a
	/// table of appearance rates to tune, because the weights are already there.
	/// </para>
	/// </remarks>
	/// <summary>
	/// How likely one rite is to be the one that called something, as the root of the dread it
	/// is pulling in.
	/// </summary>
	/// <remarks>
	/// The root, not the pressure itself, and the difference is the whole distribution. Dread
	/// rates span seventy times from the first rite to the last, so weighting them linearly
	/// gave the deepest visitor 42% of every encounter and the shallowest 0.6% - measured, not
	/// guessed - which is a table of eight with two entries that matter. Compressing it keeps
	/// the ordering honest, deeper still means likelier, while leaving every rite a real share
	/// of the door.
	/// </remarks>
	private static double VisitorWeight(int rite)
		=> Math.Sqrt(Owned[rite] * Content.Rites[rite].DreadRate);

	private static int DrawVisitor()
	{
		double total = 0.0;
		for (int i = 0; i < Content.RiteCount; i++)
		{
			total += VisitorWeight(i);
		}
		if (total <= 0.0)
		{
			return -1;
		}
		double roll = Rng.NextDouble() * total;
		for (int i = 0; i < Content.RiteCount; i++)
		{
			roll -= VisitorWeight(i);
			if (roll <= 0.0)
			{
				return i;
			}
		}
		// Only reachable on a floating-point hair; the last contributing rite is the honest
		// answer rather than a throw.
		return DeepestRite();
	}

	/// <summary>
	/// Answer whatever is walking. Returns false when the keeper cannot pay for the answer they
	/// chose, so a button can be disabled rather than a press silently doing nothing.
	/// </summary>
	/// <remarks>
	/// A WRONG answer is worse than no answer, and that asymmetry is the whole design. If
	/// guessing were free the correct play would be to mash the cheapest verb every time and
	/// collect the wins, so standing still - the answer that costs nothing - has to be able to
	/// go badly. Knowing what is coming beats guessing, guessing loses to doing nothing, and
	/// doing nothing is exactly as safe as it has always been.
	/// </remarks>
	public static bool Give(Answer answer)
	{
		if (!Approaching || answer == Answer.None)
		{
			return false;
		}
		switch (answer)
		{
			case Answer.Ward when Wards <= 0:
				return false;
			case Answer.Ward:
				Wards--;
				break;
			case Answer.Offer when Ichor < OfferCost:
				return false;
			case Answer.Offer:
				Ichor -= OfferCost;
				break;
			default:
				// The bell and standing still cost nothing to attempt. Being wrong is the price.
				break;
		}
		Resolve(answer);
		return true;
	}

	/// <summary>
	/// Something arrived, and this is what it found.
	/// </summary>
	/// <remarks>
	/// It takes the PURSE, never the parish. Losing rites is unrecoverable progress loss for
	/// a player who stepped away from a game designed to be stepped away from; losing unspent
	/// ichor is a setback the next few minutes of idling repair. The tension survives - you
	/// still lose something you wanted - and the punishment now lands on the player who
	/// pushed their luck rather than the one who went to make a coffee.
	/// </remarks>
	/// <remarks>
	/// It takes the PURSE, never the parish. Losing rites is unrecoverable progress loss for
	/// a player who stepped away from a game designed to be stepped away from; losing unspent
	/// ichor is a setback the next few minutes of idling repair. The tension survives - you
	/// still lose something you wanted - and the punishment now lands on the player who
	/// pushed their luck rather than the one who went to make a coffee.
	/// </remarks>
	private static void Resolve(Answer given)
	{
		int rite = ApproachRite;
		Answer wanted = CorrectAnswer;
		ApproachRite = -1;
		ApproachSeconds = 0.0;
		Revision++;

		// Answered correctly. Turned away without the parish paying for it, and the keeper is
		// left high on the meter - still earning at the top of the bargain, which is the prize
		// for having learned what this one is.
		if (given != Answer.None && given == wanted)
		{
			// ABOVE the 0.55 a ward leaves behind, and that ordering is load-bearing. Turning
			// something away by naming it used to drop the keeper to 0.45 - lower than simply
			// letting a ward eat it - so on the one axis that actually pays, knowing the answer
			// was a penalty. For the two visitors a ward is the answer TO, it was strictly
			// worse than not knowing: same ward spent, less dread left. A keeper who did not
			// flinch should still be standing where they were.
			Dread = 0.62;
			BeginSurge(6.0, 1.35, fromCongregation: false);
			VisitorsAnswered++;
			VisitorsBested[rite] = true;
			Say(Content.Rites[rite].VisitorName + " is turned away. You knew what it wanted.", Omen.Good);
			OnVisitation?.Invoke(true);
			return;
		}

		// Answered WRONGLY. Whatever was spent is spent, and it noticed you anyway - so no ward
		// can be reached for now. That is the entire cost of guessing.
		if (given != Answer.None)
		{
			Dread = 0.0;
			AftermathSeconds = Aftermath();
			TimesTaken++;
			Say(Content.Rites[rite].VisitorName + " was not looking for that.  " +
				Content.Rites[rite].TakenLine, Omen.Taken);
			OnVisitation?.Invoke(false);
			return;
		}

		// Nobody answered - the original behaviour, unchanged, because this is the branch an
		// absent keeper always lands on and it must cost them exactly what it always did.
		//
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
		AftermathSeconds = Aftermath();
		TimesTaken++;

		string flavour = rite >= 0 ? Content.Rites[rite].TakenLine : "Something walks the empty parish, and finds only you.";
		Say(flavour + "  The parish works at half pace for " + (int)AftermathSeconds + "s.", Omen.Taken);
		OnVisitation?.Invoke(false);
	}

	/// <summary>
	/// How long a parish is left reeling, as a share of the window between visitations.
	/// </summary>
	/// <remarks>
	/// A flat thirty seconds was fine while visitations were minutes apart and ruinous once
	/// they were not: a deep parish reaches the top of the meter every nine seconds, so a
	/// thirty-second aftermath simply never ended and half pace became the permanent rate.
	/// Measured against the window instead, being caught costs the same fraction of a keeper's
	/// progress whatever they own, and it can never stack into a state you cannot climb out of.
	/// </remarks>
	private static double Aftermath() => Math.Clamp(VisitationWindow * kAftermathShare, 8.0, 45.0)
		* (1.0 - BoonFactor(Boon.ColdBlood));

	private const double kAftermathShare = 0.4;

	/// <summary>What eating a visitation costs, as a share of a window's income: the aftermath
	/// runs at half pace, so it forfeits half of however long it lasts. Quoted so the ward's
	/// price can be checked against the thing it is an alternative to - the two are supposed to
	/// be close, or one of them is not a choice.</summary>
	public static double AftermathShareOfWindow => Aftermath() * 0.5 / VisitationWindow;

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

	/// <summary>How long the parish keeps working unattended: eight hours, and four more for
	/// every level of Unsleeping.</summary>
	public static double OfflineCapSeconds => (8.0 + 4.0 * BoonFactor(Boon.Unsleeping)) * 3600.0;

	/// <summary>What share of its usual pace the parish keeps while nobody is watching.</summary>
	public static double OfflineEfficiency => 0.5 + 0.1 * BoonFactor(Boon.Unsleeping);

	/// <summary>Replay the time a save was closed for. Chunked rather than applied in one step
	/// because production compounds through overseers - a single huge delta would buy nothing
	/// and under-pay a parish that would have been growing the whole time.</summary>
	public static OfflineReport CatchUp(double seconds)
	{
		double clamped = Math.Clamp(seconds, 0.0, OfflineCapSeconds);
		// Measured off LIFETIME, not off the purse. Overseers spend while the keeper is away -
		// that is what they are for - so the purse can easily be SMALLER on return than it was
		// on leaving, and a report drawn from it tells a keeper who came back to a parish half
		// again as productive that they gathered almost nothing. Lifetime only ever counts what
		// was produced, which is the number the sentence is actually claiming.
		double before = LifetimeIchor;
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
			Capped = seconds > OfflineCapSeconds,
			Ichor = LifetimeIchor - before,
			Dread = Dread - dreadBefore,
		};
	}

	/// <summary>Which offerings the keeper has already been told about. Not saved: it is primed
	/// from whatever is available the first time it runs, so a reload re-primes silently instead
	/// of announcing thirty things at once.</summary>
	private static bool[] s_offeringsNoticed = new bool[Content.Offerings.Length];
	private static bool s_offeringsPrimed;

	/// <summary>
	/// Say when the parish starts asking for something new.
	/// </summary>
	/// <remarks>
	/// An offering is a permanent doubling or tripling, and it simply APPEARS in a tab the
	/// keeper may not have open - nothing announced it, so the only way to know was to go and
	/// look. That is the same fault relics had: a reward that has to be discovered by polling
	/// is not a reward, it is homework. Low volume by nature, so it costs the feed almost
	/// nothing: there are thirty-odd offerings across a whole run.
	/// </remarks>
	private static void NoticeOfferings()
	{
		if (s_offeringsNoticed.Length != Content.Offerings.Length)
		{
			s_offeringsNoticed = new bool[Content.Offerings.Length];
			s_offeringsPrimed = false;
		}

		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			bool available = OfferingAvailable(i);
			if (!available)
			{
				// Taken, or no longer offered: let it be announced again if it ever returns.
				s_offeringsNoticed[i] = false;
				continue;
			}
			if (s_offeringsNoticed[i])
			{
				continue;
			}
			s_offeringsNoticed[i] = true;
			if (s_offeringsPrimed)
			{
				Say("The parish asks for something: " + Content.Offerings[i].Name + ".", Omen.Good);
			}
		}
		s_offeringsPrimed = true;
	}

	/// <summary>The last thing said, so nothing follows itself.</summary>
	private static string s_lastSpoken = "";

	private static void Say(string line, Omen omen)
	{
		s_lastSpoken = line;
		// Anything said at all counts as the parish having spoken, so the ambient voice only
		// ever fills real silence rather than talking over the game.
		s_quiet = 0.0;
		// Kept before it is shown, and kept HERE rather than in the feed, because this is the
		// one place everything the parish says passes through. Recording it where it is drawn
		// would have missed every line spoken while the feed was full, which is exactly the
		// busy stretch a keeper is most likely to have looked away during.
		Transcript.Add(line, omen, PlayedSeconds);
		Announce?.Invoke(line, omen);
	}

	/// <summary>Highest murmur band already spoken this climb, or -1. Re-arms as dread falls,
	/// so a keeper who rides the meter up and down hears the parish each time rather than
	/// once ever.</summary>
	private static int s_murmurBand = -1;

	/// <summary>Seconds since anything was said.</summary>
	private static double s_quiet;

	/// <summary>How many times the parish has pointed the keeper at the dark.</summary>
	private static int s_beckons;

	/// <summary>The nudge is spoken twice and then never again. Said into every silence it
	/// qualified for, it landed eight times in ten minutes - which is a tutorial popup with
	/// atmosphere painted on, and reads as nagging rather than as a place. Twice is enough to
	/// be noticed and few enough to stay a suggestion.</summary>
	private const int kMaxBeckons = 2;

	/// <summary>How far below a band dread must fall before that band will speak again.</summary>
	private const double kMurmurHysteresis = 0.10;

	/// <summary>How many times each dread band will say its piece in a run. They exist to teach
	/// the bargain, and a lesson repeated past learning is noise.</summary>
	private const int kMurmursPerBand = 2;

	private static int[] s_murmursSaid = new int[Content.Murmurs.Length];

	/// <summary>Which ambient line was last spoken, so the next draw can exclude it.</summary>
	private static int s_lastAmbient = -1;

	/// <summary>How long a silence has to run before the parish fills it. Long enough that it
	/// never talks over play, short enough that a dead stretch is never truly dead.</summary>
	private const double kAmbientSeconds = 55.0;

	/// <summary>
	/// Let the parish speak: on the way up the meter, and into a long enough quiet.
	/// </summary>
	/// <remarks>
	/// Offline never speaks - replaying eight hours would otherwise dump every band and a
	/// hundred ambient lines into the feed the moment a keeper returned.
	/// </remarks>
	private static void Murmur(double deltaSeconds)
	{
		int band = -1;
		for (int i = 0; i < Content.Murmurs.Length; i++)
		{
			if (Dread >= Content.Murmurs[i].At)
			{
				band = i;
			}
		}
		if (band > s_murmurBand)
		{
			s_murmurBand = band;
			// Each band speaks a few times and then never again. Hysteresis alone could not save
			// this: an answered encounter drops dread from the brink to 0.45, so every band
			// re-armed on every cycle and the same three lines replayed on a thirty-second loop
			// forever. They are TEACHING lines - a keeper who has been told twice that they are
			// earning more than they ever have does not need telling a hundredth time - so the
			// honest limit is a count, not a gap.
			if (s_murmursSaid[band] < kMurmursPerBand)
			{
				s_murmursSaid[band]++;
				Say(Content.Murmurs[band].Line, Omen.Dread);
			}
			return;
		}
		// Re-armed only when dread falls a clear margin BELOW the band it last spoke at. Without
		// the margin, a keeper hovering on a boundary - which is exactly what stoking and being
		// visited do, since both drop dread onto one - hears the same four lines over and over.
		// Atmosphere repeated on a loop stops being atmosphere faster than anything else here.
		if (band < s_murmurBand && Dread < Content.Murmurs[s_murmurBand].At - kMurmurHysteresis)
		{
			s_murmurBand = band;
		}

		s_quiet += deltaSeconds;
		if (s_quiet < kAmbientSeconds)
		{
			return;
		}
		// A keeper who has never met anything is told, once the silence is long enough, that
		// meeting something is a thing they can choose. Everyone else just gets the parish.
		bool neverLooked = VisitorsAnswered == 0 && !Approaching && Dread < 0.5 && DeepestRite() >= 0;
		for (int i = 0; i < VisitorsMet.Length && neverLooked; i++)
		{
			neverLooked &= !VisitorsMet[i];
		}
		// ...and not if it was the last thing said. The nudge is capped at two, but nothing
		// stopped those two being consecutive - the ambient draw avoids repeating itself and the
		// beckon simply bypassed that rule. A line delivered twice running reads as a bug even
		// when it is only a coincidence of timing.
		if (neverLooked && s_beckons < kMaxBeckons && s_lastSpoken != Content.Beckon)
		{
			s_beckons++;
			Say(Content.Beckon, Omen.Plain);
			return;
		}
		// Never the same line twice running. A uniform draw over eight lines repeats about one
		// time in eight, which at this cadence means hearing the ossuary shift twice inside two
		// minutes - and a repeated atmospheric line stops being atmosphere and starts being a
		// string in an array.
		int pick = Rng.Next(Content.Ambient.Length - 1);
		if (pick >= s_lastAmbient)
		{
			pick++;
		}
		s_lastAmbient = pick;
		Say(Content.Ambient[pick], Omen.Plain);
	}

	/// <summary>Wipe live state back to a fresh vigil. Used by a new game and by the tests.</summary>
	public static void Reset()
	{
		// A fresh vigil starts with nothing said. Note this is Reset and not Commune: a keeper
		// who communes is the SAME keeper carrying on, and throwing away what the parish has
		// told them at the one moment the game gets most interesting would be perverse.
		Transcript.Clear();
		Consecrated = -1;
		Ichor = 0.0;
		RunIchor = 0.0;
		LifetimeIchor = 0.0;
		Array.Clear(Owned, 0, Owned.Length);
		OfferingsTaken = new bool[Content.Offerings.Length];
		MarksEarned = new bool[Content.Marks.Length];
		Overseers = new bool[Content.RiteCount];
		Boons = new int[Content.Boons.Length];
		Sigils = 0;
		SigilsEarned = 0;
		Wards = 0;
		AftermathSeconds = 0.0;
		StokeCooldown = 0.0;
		Fervour = 0.0;
		Communions = 0;
		Dread = 0.0;
		PlayedSeconds = 0.0;
		HighDreadSeconds = 0.0;
		WardsRaised = 0;
		TimesTaken = 0;
		VisitorsAnswered = 0;
		VisitorsMet = new bool[Content.RiteCount];
		VisitorsBested = new bool[Content.RiteCount];
		s_murmurBand = -1;
		s_quiet = 0.0;
		s_beckons = 0;
		s_lastAmbient = -1;
		s_lastStokeLine = 0.0;
		// Both call sites - a fresh vigil AND a communion. Communion clears the offerings
		// taken, so every one of them becomes available again at once; without re-priming, a
		// keeper would be told about thirty offerings in a single frame.
		s_offeringsNoticed = new bool[Content.Offerings.Length];
		s_offeringsPrimed = false;
		s_murmursSaid = new int[Content.Murmurs.Length];
		s_lastSpoken = "";
		CommunionSurges = 0;
		ApproachRite = -1;
		ApproachSeconds = 0.0;
		HandGathers = 0;
		SharedVigilSeconds = 0.0;
		SurgeSeconds = 0.0;
		SurgeMultiplier = 1.0;
		Array.Clear(CycleProgress, 0, CycleProgress.Length);
		Echoes.Clear();
		Satchel.Clear();
		Array.Clear(Worn, 0, Worn.Length);
		RelicsFound = 0;
		// Somewhere in the first billion, leaving room to count up without ever wrapping into
		// another keeper's stretch of the space.
		RelicSeed = Rng.Next(1, 1_000_000_000);
		Revision++;
	}
}

/// <summary>One keeper this one used to be, and what they are holding.</summary>
public struct Echo
{
	public string Name;
	/// <summary>0 to <see cref="Vigil.kEchoCapacity"/>. Dread that left the living keeper and
	/// did not stop existing.</summary>
	public double Burden;
}

/// <summary>What the keeper missed while the game was closed.</summary>
public struct OfflineReport
{
	public double Seconds;
	public bool Capped;
	public double Ichor;
	public double Dread;
}
