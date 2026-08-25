using System;
using System.Collections.Generic;
using System.Numerics;

namespace AetherGame;

/// <summary>
/// What a keeper can do about something walking toward them.
/// </summary>
/// <remarks>
/// Four verbs, and every one of them is a thing the game could already do - spend a ward,
/// pay ichor, ring the bell, or stand still and let it pass. Nothing here is a new system;
/// it is the existing controls given a moment where WHICH one you reach for matters.
/// </remarks>
public enum Answer
{
	/// <summary>Nobody answered. The keeper was away, and the vigil defends itself.</summary>
	None,
	/// <summary>Put a ward between you.</summary>
	Ward,
	/// <summary>Give it something so it takes that instead.</summary>
	Offer,
	/// <summary>Drown it out.</summary>
	Bell,
	/// <summary>Do not move. Some of them find you by looking.</summary>
	Still,
}

/// <summary>One buyable producer. Immutable data; everything that changes lives in
/// <see cref="Vigil"/> so a save is a list of numbers rather than a graph of objects.</summary>
public sealed class RiteDef
{
	public string Name = "";
	public string Blurb = "";
	/// <summary>Cost of the first one. Each subsequent copy costs <see cref="Growth"/> times more.</summary>
	public double BaseCost;
	/// <summary>Ichor per second, per owned copy, before any multiplier.</summary>
	public double BaseRate;
	/// <summary>Cost growth per copy owned. Higher = the tier runs out of road sooner.</summary>
	public double Growth;
	/// <summary>Dread added per second, per owned copy. The whole tension of the game: the
	/// deeper rites pay far better and pull the dark in far faster.</summary>
	public double DreadRate;
	/// <summary>Seconds one working of this rite takes. A rite does not trickle - it WORKS,
	/// and then it delivers, so buying one buys a thing you can watch do something. Deeper
	/// rites are slower and pay far more per working, which is what makes the parish read as
	/// a set of machines running at different speeds rather than one number going up.</summary>
	public double CycleSeconds;
	/// <summary>Tint for this rite in the world and in the ledger. Not stored: it is a step
	/// on the one ramp, so a rite cannot invent a colour of its own. See Palette's remarks
	/// for why eight arbitrary hues were the problem rather than the decoration.</summary>
	public Vector4 Colour => Palette.RiteTint(Index);
	/// <summary>Position in the table, which is what the tint and the light are derived from.</summary>
	public int Index;
	/// <summary>What the parish says when one of these is taken from you.</summary>
	public string TakenLine = "";
	/// <summary>What comes for a keeper whose deepest holding is this rite. Every tier has its
	/// own, so going deeper does not only raise the stakes - it changes who arrives.</summary>
	public string VisitorName = "";
	/// <summary>The warning, a few seconds before it gets here. This is the whole encounter:
	/// the line has to be enough for a keeper who has met this one before to know what to
	/// reach for, and not enough for one who has not.</summary>
	public string Approach = "";

	/// <summary>
	/// Other ways this one announces itself.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The encounter is the most dramatic thing in the game and each visitor said the same
	/// sentence every single time it came, which turns a warning into a label. These are drawn
	/// from instead, once per approach.
	/// </para>
	/// <para>
	/// Every variant keeps the TELL. The line is the only thing a keeper has to work out what
	/// the thing wants, and the whole progression of the encounter lives in the player learning
	/// to read it - so a variant that dropped or moved the cue would not be flavour, it would be
	/// the game cheating. Each of these says the same thing about what is coming in different
	/// words. <see cref="Approach"/> itself stays the canonical one, and is what the bestiary
	/// shows, so what a keeper studies is stable even though what they hear is not.
	/// </para>
	/// </remarks>
	public string[] AlsoApproach = Array.Empty<string>();

	/// <summary>Other ways the parish reports having lost this one. Unlike the approach lines
	/// these carry no tell - nothing is being learned at the moment something is taken - so they
	/// are flavour and nothing else, and can be drawn freely.</summary>
	public string[] AlsoTaken = Array.Empty<string>();
	/// <summary>The one thing that turns this visitor away. Learned by meeting it, which is
	/// the only progression in the game that lives in the player rather than in the save.</summary>
	public Answer Answer = Answer.None;
	/// <summary>The sprite that stands for this rite in the parish. Drawn in greyscale and
	/// tinted by <see cref="Colour"/> at runtime, so one sprite serves both the lit and the
	/// dread-soured version of the same thing.</summary>
	public string Art = "";
}

/// <summary>A one-off purchase that multiplies something. <see cref="Target"/> is a rite
/// index, or <see cref="TargetGlobal"/>, or <see cref="TargetHand"/>.</summary>
public sealed class OfferingDef
{
	public const int TargetGlobal = -1;
	public const int TargetHand = -2;

	public string Name = "";
	public string Blurb = "";
	public double Cost;
	public int Target;

	/// <summary>
	/// What this multiplies its target's output by.
	/// </summary>
	/// <remarks>
	/// Defaults to 1, not 0. Every offering used to be a multiplier so every entry set it, and a
	/// field that is always written needs no default - but the offerings below are no longer all
	/// multipliers, and one that only changes a rite's SPEED would otherwise have multiplied its
	/// output by zero and silently switched the rite off.
	/// </remarks>
	public double Multiplier = 1.0;

	/// <summary>
	/// What this multiplies its rite's dread contribution by. Below 1 is quieter.
	/// </summary>
	/// <remarks>
	/// The most interesting knob in the game to hand to a player, because it plays directly
	/// against the central bargain: the deep rites pay far better AND pull the dark in far
	/// faster, and this lets a keeper buy off part of the second half of that sentence. It never
	/// buys off all of it - see the floor in <c>Vigil.RiteDreadScale</c> - because a parish that
	/// can be made safe is a parish with nothing left to decide.
	/// </remarks>
	public double DreadScale = 1.0;

	/// <summary>Copies of <see cref="Target"/> needed before this is offered. Global and
	/// hand offerings use <see cref="LifetimeNeeded"/> instead.</summary>
	public int OwnedNeeded;
	public double LifetimeNeeded;
}

/// <summary>
/// A permanent sigil purchase, kept through every communion.
/// </summary>
/// <remarks>
/// Every boon turns a knob the game already has - the ward cap, the aftermath, what dread
/// pays, how fast fervour drains, the offline cap, the stoke cooldown - rather than adding a
/// system of its own. That is deliberate: a sigil should make the vigil you already know how
/// to play deeper, not hand you a second game to learn. It also means a boon cannot fall out
/// of step with the thing it modifies, because there is only ever one of them.
/// </remarks>
public sealed class BoonDef
{
	public string Name = "";
	public string Blurb = "";
	/// <summary>Sigils for the first level. Each level after costs <see cref="Growth"/> times
	/// more, so a boon runs out of road on its own rather than needing a hand-written price
	/// per level that some later edit forgets to keep in order.</summary>
	public double BaseCost;
	public double Growth = 2.4;
	/// <summary>
	/// How far this boon goes, or <see cref="Endless"/> for one that never finishes.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Most of them end at three. A ward cap that keeps rising stops being a cap; an aftermath
	/// that keeps shrinking stops being a consequence. These are QUALITATIVE - each changes how
	/// the vigil is played, and there are only so many meaningful steps in that.
	/// </para>
	/// <para>
	/// One of them has a very long ladder instead, which is the prestige sink - see The Old
	/// Bargain. It is a LONG ladder and not a special case: a sentinel meaning "no ceiling" was
	/// tried and it was a mistake, because MaxLevel is arithmetic in eight other places and a
	/// negative one poisoned all of them at once. It read as already maxed in the ledger, drew a
	/// progress bar of negative width, printed "3/-1", and divided by zero in a fixture. A
	/// number that is simply large breaks nothing and says the same thing.
	/// </para>
	/// </remarks>
	public int MaxLevel = 3;

	/// <summary>How much one level is worth, in whatever unit the boon deals in. Read by the
	/// one place in <see cref="Vigil"/> that applies it.</summary>
	public double Step;
}

/// <summary>Something the keeper did, remembered across communion and across sessions.
/// None of them pay out - a vigil is a record, not a wage.</summary>
public sealed class MarkDef
{
	public string Name = "";
	public string Blurb = "";
	public Func<bool> Earned = () => false;
	/// <summary>True for a mark no keeper can earn on their own. Flagged rather than hidden:
	/// a solo player should be able to see that the two they cannot reach need other people,
	/// instead of reading them as ones they have simply not managed yet - and it is the only
	/// thing on the vigil screen that tells them the game has a congregation at all.</summary>
	public bool NeedsCongregation;

	/// <summary>
	/// How far along this mark is, from 0 to 1, or null when it cannot be halfway.
	/// </summary>
	/// <remarks>
	/// Twenty-three marks with nothing but a name and a sentence read as a list of things you
	/// have not done. The ones that COUNT something - twenty visitors turned away, fifty relics
	/// dug up, forty offerings standing - are a track, and showing where a keeper is on it is
	/// the difference between a goal and a reproach.
	/// <para>
	/// Null for the marks that genuinely cannot be partly done: lighting a first lantern is not
	/// 40% complete. A bar on those would be theatre.
	/// </para>
	/// </remarks>
	public Func<double>? Toward;
}

/// <summary>Every table the game is made of. Static data only.</summary>
public static class Content
{
	public const int RiteCount = 8;

	/// <summary>Copies of a rite that double its output, again and again. Free, automatic,
	/// and the reason a tier you have outgrown is still worth topping up.</summary>
	public const int MilestoneStep = 25;

	public static readonly RiteDef[] Rites = NumberThem(new RiteDef[]
	{
		new RiteDef
		{
			Name = "Grave Lantern", Art = "project://assets/textures/rites/grave_lantern.png", CycleSeconds = 2.5,
			Blurb = "It burns low, and something moves at the edge of it.",
			BaseCost = 15.0, BaseRate = 0.1, Growth = 1.13, DreadRate = 0.0010,
			TakenLine = "A lantern goes out. You did not hear it fall.",
			AlsoTaken = new[]
			{
				"One of the lanterns is dark, and cold, and has been for longer than that.",
				"There is a lantern missing from the line. The bracket is bent outward.",
			},
			VisitorName = "The Wick-Thin Man", Answer = Answer.Still,
			Approach = "Something thin is walking the lantern line, and stopping at each one.",
			AlsoApproach = new[]
			{
				"There is a thin shape between the lanterns. It stops whenever you do.",
				"Something narrow is going along the lights, pausing at every one of them.",
			},
		},
		new RiteDef
		{
			Name = "Bone Choir", Art = "project://assets/textures/rites/bone_choir.png", CycleSeconds = 3.5,
			Blurb = "Twelve throats, no air, and they keep perfect time.",
			BaseCost = 110.0, BaseRate = 0.9, Growth = 1.14, DreadRate = 0.0022,
			TakenLine = "The choir drops a voice. The others do not adjust.",
			AlsoTaken = new[]
			{
				"The choir is one quieter. Nobody has moved to fill the gap.",
				"A voice stops. The rest sing the same as they did with it.",
			},
			VisitorName = "The Thirteenth Voice", Answer = Answer.Bell,
			Approach = "A voice joins the choir. It is holding a note none of them started.",
			AlsoApproach = new[]
			{
				"The choir has gained a voice. It is singing something they did not begin.",
				"There is one more note in the choir than there are throats for.",
			},
		},
		new RiteDef
		{
			Name = "Weeping Statue", Art = "project://assets/textures/rites/weeping_statue.png", CycleSeconds = 5.0,
			Blurb = "You have never seen it move. It is never where it was.",
			BaseCost = 1300.0, BaseRate = 7.0, Growth = 1.15, DreadRate = 0.0044,
			TakenLine = "A plinth stands empty. The stains lead away from it.",
			AlsoTaken = new[]
			{
				"One plinth has nothing on it. The marks go off toward the door.",
				"Something has stepped down off its stone. It did not go far in a straight line.",
			},
			VisitorName = "The Unmoved", Answer = Answer.Still,
			Approach = "The statue is facing the other way. You are certain it is watching.",
			AlsoApproach = new[]
			{
				"The statue has turned from you, and you have never been more sure of being watched.",
				"It is facing the wall now. It is still looking at you.",
			},
		},
		new RiteDef
		{
			Name = "Flesh Loom", Art = "project://assets/textures/rites/flesh_loom.png", CycleSeconds = 7.0,
			Blurb = "It asks for very little and it never stops asking.",
			BaseCost = 15000.0, BaseRate = 44.0, Growth = 1.15, DreadRate = 0.0080,
			TakenLine = "The loom is unthreaded. Something wore what it made.",
			AlsoTaken = new[]
			{
				"The loom hangs bare. What came off it is being worn somewhere.",
				"The weave is gone from the frame, and gone from the room.",
			},
			VisitorName = "The Unthreaded", Answer = Answer.Offer,
			Approach = "Something is pulling at the weave, and it is hungry rather than cruel.",
			AlsoApproach = new[]
			{
				"Something is tugging at the loom. It wants, rather than hates.",
				"The weave is being drawn at by something starving. It has asked for nothing yet.",
			},
		},
		new RiteDef
		{
			Name = "Ossuary Engine", Art = "project://assets/textures/rites/ossuary_engine.png", CycleSeconds = 9.0,
			Blurb = "Built from the parish it drains. It is very efficient.",
			BaseCost = 190000.0, BaseRate = 260.0, Growth = 1.16, DreadRate = 0.0140,
			TakenLine = "An engine seizes. The bones in it were not ours.",
			AlsoTaken = new[]
			{
				"An engine stops hard. What was inside it never belonged to this parish.",
				"One of the engines has locked up around something it should not have held.",
			},
			VisitorName = "The Millwright", Answer = Answer.Offer,
			Approach = "The engine is running faster than you set it. Something is feeding it.",
			AlsoApproach = new[]
			{
				"The engine has sped up. Something is putting more in than you did.",
				"You did not set it this fast. Something is giving it more to work with.",
			},
		},
		new RiteDef
		{
			Name = "Drowned Chapel", Art = "project://assets/textures/rites/drowned_chapel.png", CycleSeconds = 12.0,
			Blurb = "The tide keeps the congregation. The congregation keeps singing.",
			BaseCost = 2600000.0, BaseRate = 1500.0, Growth = 1.16, DreadRate = 0.0240,
			TakenLine = "A chapel slips under. The singing does not stop, only muffles.",
			AlsoTaken = new[]
			{
				"A chapel goes down under the water. The song keeps on, thicker.",
				"Water closes over a chapel. Whatever is singing does not come up.",
			},
			VisitorName = "The Tide-Sung", Answer = Answer.Bell,
			Approach = "The water in the nave is rising, and the singing is getting louder.",
			AlsoApproach = new[]
			{
				"The nave is filling, and whatever is singing is singing harder for it.",
				"Water over the flags, and the song under it climbing.",
			},
		},
		new RiteDef
		{
			Name = "Pale Shepherd", Art = "project://assets/textures/rites/pale_shepherd.png", CycleSeconds = 16.0,
			Blurb = "It gathers what wanders. You have agreed not to wander.",
			BaseCost = 42000000.0, BaseRate = 8800.0, Growth = 1.17, DreadRate = 0.0420,
			TakenLine = "A shepherd walks off with its flock. Count yourself.",
			AlsoTaken = new[]
			{
				"A shepherd leaves, and takes its count with it. You are still here to be counted.",
				"One shepherd is gone, and so is everything it was numbering.",
			},
			VisitorName = "The Shepherd's Count", Answer = Answer.Ward,
			Approach = "It has begun counting the flock. Do not let it reach you.",
			AlsoApproach = new[]
			{
				"Something is going along the parish counting. You are near the end of the row.",
				"It is numbering everything here, one at a time, and coming your way.",
			},
		},
		new RiteDef
		{
			Name = "Hollow Mouth", Art = "project://assets/textures/rites/hollow_mouth.png", CycleSeconds = 22.0,
			Blurb = "It is not a door. Doors are for going back through.",
			BaseCost = 720000000.0, BaseRate = 51000.0, Growth = 1.18, DreadRate = 0.0700,
			TakenLine = "A mouth closes. You are certain it swallowed.",
			AlsoTaken = new[]
			{
				"A mouth shuts. Something went down with it.",
				"The mouth is closed now, and fuller than it was.",
			},
			VisitorName = "What Came Through", Answer = Answer.Ward,
			Approach = "The mouth is open wider than it opens. Something is using it as a door.",
			AlsoApproach = new[]
			{
				"The mouth has opened past its own hinge. Something is coming through it.",
				"It is open further than it can open. Something is treating it as a way in.",
			},
		},
	});

	/// <summary>Stamps each rite with its position, so the palette can derive its step on the
	/// ramp without every entry repeating a number that must match its own index.</summary>
	private static RiteDef[] NumberThem(RiteDef[] rites)
	{
		for (int i = 0; i < rites.Length; i++)
		{
			rites[i].Index = i;
		}
		return rites;
	}

	/// <summary>
	/// Everything the parish will ask for.
	/// </summary>
	/// <remarks>
	/// <b>The first thirty entries may never move.</b> A save stores which offerings have been
	/// taken as a bare array of flags indexed by position, so reordering this table silently
	/// re-points every flag in every existing save at a different offering - a keeper would load
	/// their game and find they had bought things they never bought. Anything new goes on the
	/// END, which is why the deep ladder below is a separate pass rather than more steps woven
	/// into the first one. There is a check in the balance harness that holds this.
	/// </remarks>
	public static readonly OfferingDef[] Offerings = BuildOfferings();

	private static OfferingDef[] BuildOfferings()
	{
		List<OfferingDef> list = new List<OfferingDef>();

		// The ladder reads as things you do TO a rite, because the fiction is that you are
		// not upgrading equipment - you are feeding it.
		string[] ladderName = { "Wick of Hair", "Second Wick", "Vigil Oil" };
		string[] ladderBlurb =
		{
			"Trim it with something that grew on a person.",
			"It burns twice as well and half as honestly.",
			"Rendered on the third night. Do not ask from what.",
		};
		int[] needed = { 10, 25, 50 };
		double[] mult = { 2.0, 2.0, 3.0 };
		double[] costFactor = { 12.0, 120.0, 2200.0 };

		for (int rite = 0; rite < Rites.Length; rite++)
		{
			for (int step = 0; step < ladderName.Length; step++)
			{
				list.Add(new OfferingDef
				{
					Name = Rites[rite].Name + ": " + ladderName[step],
					Blurb = ladderBlurb[step],
					Cost = Rites[rite].BaseCost * costFactor[step],
					Target = rite,
					Multiplier = mult[step],
					OwnedNeeded = needed[step],
				});
			}
		}

		// Global and by-hand offerings sit on a lifetime ladder, so they arrive regardless
		// of which tier the keeper leaned on to get there.
		list.Add(new OfferingDef
		{
			Name = "Steady Hands", Blurb = "Every gathering by hand takes twice as much.",
			Cost = 500.0, Target = OfferingDef.TargetHand, Multiplier = 2.0, LifetimeNeeded = 300.0,
		});
		list.Add(new OfferingDef
		{
			Name = "Bitten Tongue", Blurb = "You stop counting what you take. It goes faster.",
			Cost = 40000.0, Target = OfferingDef.TargetHand, Multiplier = 5.0, LifetimeNeeded = 20000.0,
		});
		list.Add(new OfferingDef
		{
			Name = "Red Thumb", Blurb = "The old cut never closed. Ten times by hand.",
			Cost = 9000000.0, Target = OfferingDef.TargetHand, Multiplier = 10.0, LifetimeNeeded = 3000000.0,
		});
		list.Add(new OfferingDef
		{
			Name = "The Long Hour", Blurb = "Everything in the parish gives half again.",
			Cost = 12000.0, Target = OfferingDef.TargetGlobal, Multiplier = 1.5, LifetimeNeeded = 8000.0,
		});
		list.Add(new OfferingDef
		{
			Name = "Names in the Ledger", Blurb = "Everything gives double. The list is long now.",
			Cost = 4000000.0, Target = OfferingDef.TargetGlobal, Multiplier = 2.0, LifetimeNeeded = 1500000.0,
		});
		list.Add(new OfferingDef
		{
			Name = "The Parish Remembers", Blurb = "Everything gives triple. Nothing has forgotten you.",
			Cost = 900000000.0, Target = OfferingDef.TargetGlobal, Multiplier = 3.0, LifetimeNeeded = 400000000.0,
		});

		// ── Everything past here was added later, and must stay past here ────────────────
		// See the note on Offerings: a save indexes this table by position.
		//
		// THREE kinds of offering beyond a plain multiplier were tried here and two were thrown
		// away, both because of what they did to the balance rather than to the code. Recorded
		// so the next one does not have to learn it twice:
		//
		//   - A rite that WORKS FASTER (a quarter off its cycle) is a third more ichor at no cost
		//     in dread, and that gain lands on a keeper who is not yet against any ceiling. It cut
		//     the reward for patience at the communion from 9.8x to 5.6x. A gain is not neutral;
		//     what matters is WHOSE.
		//   - A rite whose free doublings come every TWENTY copies instead of twenty-five looks
		//     like the mildest entry on this list and is by far the strongest thing ever put in
		//     it. The doublings are an exponent, so shortening its divisor multiplies output by
		//     2^(owned/20 - owned/25) - four times over at two hundred copies and climbing
		//     forever. It broke the floor under stoking and what leaning on an echo pays, at
		//     once. Never buy an exponent with a one-off purchase.
		//
		// What survived is the one that costs the keeper something. That is not a coincidence:
		// this game is a single bargain, and an offering that only gives is an offering that
		// flattens it.

		// The deep ladder. The first three steps run out at fifty copies of a rite, which a
		// keeper who leans on one tier passes inside an hour - and then the tab is empty and
		// says to go and keep more of a rite they already have hundreds of. These carry on.
		string[] deepName = { "Bone Ash", "Wound Tally", "The Long Feeding", "Grave Tithe", "Unmarked Debt" };
		string[] deepBlurb =
		{
			"What is left when the burning is done properly.",
			"One mark for each. The wall is running out of room.",
			"It has stopped needing to be asked.",
			"Paid in the only coin the ground accepts.",
			"Nobody living remembers agreeing to this.",
		};
		int[] deepNeeded = { 100, 150, 250, 400, 600 };
		// Deliberately gentle. The first attempt ran 2x 2x 2x 3x 3x - seventy-two times per rite
		// on top of the twelve the early ladder already gives - and measured, that broke two
		// things at once: a parish that rich sits near the brink on its own, so deliberately
		// stoking toward it bought almost nothing (1.1x against a floor of 1.2x), and every other
		// source in the game - what dread pays, what a relic is worth, what a congregation adds -
		// shrank against it. The complaint these answer is that the tab RUNS OUT, which is a
		// question of how many there are, not how big they are.
		double[] deepMult = { 1.5, 1.5, 2.0, 2.0, 2.0 };
		double[] deepCost = { 12000.0, 60000.0, 400000.0, 5000000.0, 100000000.0 };

		for (int rite = 0; rite < Rites.Length; rite++)
		{
			for (int step = 0; step < deepName.Length; step++)
			{
				list.Add(new OfferingDef
				{
					Name = Rites[rite].Name + ": " + deepName[step],
					Blurb = deepBlurb[step],
					Cost = Rites[rite].BaseCost * deepCost[step],
					Target = rite,
					Multiplier = deepMult[step],
					OwnedNeeded = deepNeeded[step],
				});
			}
		}

		// A rite that pulls the dark in less. The one offering in the game that makes going
		// DEEPER cheaper in the only currency that actually costs a keeper anything.
		for (int rite = 0; rite < Rites.Length; rite++)
		{
			list.Add(new OfferingDef
			{
				Name = Rites[rite].Name + ": Muffled Bell",
				Blurb = "Carries less far: a third less dread, and a seventh less taken.",
				Cost = Rites[rite].BaseCost * 6000.0,
				Target = rite,
				DreadScale = 0.70,
				// A BARGAIN, not a bonus, and it has to be. Measured, a free dread reduction cut
				// the reward for patience at the communion clean in half - from 9.8x to 4.8x -
				// because riding the meter high for a long run is precisely how patience pays,
				// and quieting the meter for nothing takes that away. The whole game is one
				// trade: the dark pays, and it is coming. An offering may let a keeper sit
				// further from it, but not for free, or there was never a decision there.
				Multiplier = 0.85,
				OwnedNeeded = 60,
			});
		}

		// And the mirror of it. The parish's whole sentence is "the dark pays, and it is coming",
		// and Muffled Bell lets a keeper buy a little distance from the second half. This sells
		// them the first half instead: more taken, and more of it coming. Two offerings pointing
		// opposite ways on the same axis is the clearest way to put the bargain in a keeper's
		// hands - and unlike everything that was thrown away, neither of them is free.
		//
		// Buying both on one rite very nearly cancels out. That is allowed. A keeper who does it
		// has spent ichor to end up where they started, which is their business, and machinery to
		// forbid it would cost more than the mistake does.
		for (int rite = 0; rite < Rites.Length; rite++)
		{
			list.Add(new OfferingDef
			{
				Name = Rites[rite].Name + ": Unhooded Lantern",
				Blurb = "Nothing between it and the dark: two fifths more taken, and half again the dread.",
				Cost = Rites[rite].BaseCost * 2500.0,
				Target = rite,
				Multiplier = 1.40,
				DreadScale = 1.50,
				OwnedNeeded = 80,
			});
		}

		return list.ToArray();
	}

	/// <summary>
	/// What sigils are for.
	/// </summary>
	/// <remarks>
	/// Overseers alone were 108 sigils of spending in a game that will hand out thousands, so
	/// every communion after the first bought nothing and the prestige currency decayed into a
	/// flat percentage. These are the long tail: expensive enough that the last levels are many
	/// runs away, and each one changes how the vigil is PLAYED rather than only how fast the
	/// number climbs.
	/// </remarks>
	public static readonly BoonDef[] Boons =
	{
		new BoonDef
		{
			Name = "Deeper Wards", Blurb = "Set one more ward aside than the last keeper could.",
			BaseCost = 6.0, MaxLevel = 3, Step = 1.0,
		},
		new BoonDef
		{
			Name = "Cold Blood", Blurb = "You recover faster from a visitation. A fifth faster, each time.",
			BaseCost = 5.0, MaxLevel = 3, Step = 0.20,
		},
		// THE SINK. Every other boon ends, and measured over a fortnight of hard play a keeper
		// earns three hundred thousand sigils against about four hundred of total spending - so
		// prestige stopped being something you spend and became a number that only accumulates,
		// which is exactly the decay the boons were added to fix, arriving again further out.
		//
		// This one never finishes. Its cost multiplies and its effect adds, so what a keeper
		// gets grows with the LOGARITHM of what they have earned: a fortnight buys about twelve
		// levels, and ten times the sigils buys three more. That shape is the point - the sink
		// never fills, and it never runs away either.
		//
		// It is this boon rather than a new one because it is the game's own sentence. Dread
		// pays, and it is coming; a keeper who buys more of the first half is choosing to want
		// the meter higher, which is a decision and not a number.
		new BoonDef
		{
			Name = "The Old Bargain", Blurb = "Dread pays better. It always did, for the ones who asked twice.",
			// Step down from 0.55 to 0.30 because the ladder no longer stops at three: the first
			// three levels are worth a little less than they were, and the fourth onward did not
			// exist at all. The ceiling is far past anywhere a keeper arrives - the cost
			// multiplies until it meets its cap, so beyond about the twentieth level each one
			// costs a flat billion sigils, and a fortnight of hard play earns three hundred
			// thousand.
			BaseCost = 10.0, MaxLevel = 200, Step = 0.30,
		},
		new BoonDef
		{
			Name = "Steady Hand", Blurb = "Fervour leaves you a quarter more slowly.",
			BaseCost = 4.0, MaxLevel = 3, Step = 0.25,
		},
		new BoonDef
		{
			Name = "Unsleeping", Blurb = "The parish keeps four more hours without you, and works harder doing it.",
			BaseCost = 6.0, MaxLevel = 3, Step = 1.0,
		},
		new BoonDef
		{
			Name = "Quick Kindling", Blurb = "You can lean on the dark again sooner.",
			BaseCost = 5.0, MaxLevel = 3, Step = 0.25,
		},
	};

	/// <summary>Pulled out of the table because a lambda with a loop in it reads badly inside
	/// a list of one-liners, and the mark is the only entry that needs to ask about all six.</summary>
	private static bool AnyBoonMaxed()
	{
		for (int i = 0; i < Boons.Length; i++)
		{
			if (Vigil.Boons[i] >= Boons[i].MaxLevel)
			{
				return true;
			}
		}
		return false;
	}

	/// <summary>
	/// What the parish says as the dark gets closer, and the level it says it at.
	/// </summary>
	/// <remarks>
	/// <para>
	/// These do the job the interface cannot. A new keeper's first ten minutes used to be six
	/// lines, every one of them a receipt - "Offered: X", "Marked: Y" - while the dread meter
	/// climbed to a third full and the game never once mentioned it. A horror game whose only
	/// voice congratulates you is not atmospheric, it is a notification tray.
	/// </para>
	/// <para>
	/// They also TEACH, which is why the wording is careful. The line at 0.20 says the dark
	/// pays; the one at 0.70 says you are earning more than you ever have and could stop. A
	/// keeper who reads those two has been told the central bargain of the game without a
	/// tutorial box ever appearing.
	/// </para>
	/// </remarks>
	public static readonly (double At, string Line)[] Murmurs =
	{
		(0.20, "Something has noticed the parish. It pays better when it is watching."),
		(0.45, "The lanterns lean away from the door. Whatever is out there is nearer."),
		(0.70, "You could stop now. You are earning more than you ever have."),
		(0.90, "It is at the edge of the light. One more step and it will be inside."),
	};

	/// <summary>
	/// What each dread band says once it has finished teaching.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The lines above are LESSONS, and a lesson is said twice and then never again - a keeper
	/// who has been told they are earning more than they ever have does not need telling a
	/// hundredth time. Correct, and it left a hole: after roughly ten minutes the dread meter,
	/// which is the whole of this game, went permanently silent. Crossing into the brink - the
	/// most dangerous thing a keeper can choose to do - said nothing at all.
	/// </para>
	/// <para>
	/// These say nothing a player needs to learn, so they can keep coming. Indexed to match
	/// <see cref="Murmurs"/> band for band, which is checked, because a band whose atmosphere
	/// belonged to a different depth would be worse than silence.
	/// </para>
	/// </remarks>
	public static readonly string[][] Deeper =
	{
		new[]
		{
			"The dark has moved a little closer to the door.",
			"Something outside is keeping pace with you.",
			"You are being read, page by page.",
		},
		new[]
		{
			"The lanterns are all leaning the same way now.",
			"There is weight on the flags outside. It is not walking.",
			"Whatever it is has stopped pretending not to be there.",
			"The cold is coming from one direction only.",
		},
		new[]
		{
			"You have never had this much. You have never been this close.",
			"The parish is working beautifully. That is the trade.",
			"It is worth it. That is the problem with it.",
			"Everything is louder. Everything is nearer.",
		},
		new[]
		{
			"It is at the edge of the light and it is not moving.",
			"You could put a hand out and touch it.",
			"The lanterns have stopped guttering. Nothing is moving at all.",
			"There is nothing between you and it now.",
		},
	};

	/// <summary>
	/// Things the parish does when nothing else is happening.
	/// </summary>
	/// <remarks>
	/// Pure atmosphere, no mechanics, and that is deliberate: an idle game has long quiet
	/// stretches by design - a first session runs thirteen minutes with one event in it - and
	/// the quiet is either dread or boredom depending entirely on whether anything fills it.
	/// Spoken only into real silence, never on top of something the player did.
	/// </remarks>
	public static readonly string[] Ambient =
	{
		"The candles gutter, all at once, and settle.",
		"Something in the ossuary shifts its weight.",
		"A door you did not open is open.",
		"The last keeper wrote in a hand very like yours.",
		"You count the lanterns twice and get different numbers.",
		"The cold comes up through the flags. It has always done that.",
		"Somewhere behind you, the singing stops to listen.",
		"There is a name in the ledger you do not remember writing.",
		"The bell rope is swinging. Nobody has touched it.",
		"You find a chair pulled out, and put it back.",
		"Water is getting in somewhere. You cannot find where.",
		"For a moment the parish sounds like it is full.",
		"Your breath shows. The night is not that cold.",
		"Something is counting along with you, one behind.",
		"The flags by the door have been worn smooth. Not by you.",
		"A moth goes into a lantern and does not come out the other side.",
		"You have been holding your breath. You do not know for how long.",
		"The floor is dry where the rain came in.",
		"Somebody has laid the table for a service. There is no service.",
		"The dark past the pillars has depth to it tonight.",
		"You hear the outer door close. You did not hear it open.",
		"There is less dust than there should be.",
		"One of the candles is burning down faster than the rest.",
		"The silence has a shape, and you are standing in the middle of it.",
	};

	/// <summary>
	/// A line, and when the parish would say it.
	/// </summary>
	/// <remarks>
	/// The atmosphere above is true of any night in the parish. These are true of THIS one - a
	/// keeper who has just consecrated a rite, or is wearing a relic that stands where nothing
	/// was built, or has been leaning on the dark all evening, hears the place remark on it.
	/// A voice that knows what you have been doing is worth a great deal more than a longer
	/// list of things it says regardless.
	/// </remarks>
	public sealed class AmbientDef
	{
		public string Line = "";
		public Func<bool> When = () => true;
	}

	public static readonly AmbientDef[] Noticed =
	{
		new AmbientDef
		{
			Line = "Whatever you gave the rite, it has started asking for it by name.",
			When = () => Vigil.Consecrated >= 0,
		},
		new AmbientDef
		{
			Line = "There are more of them standing than you remember paying for.",
			When = Vigil.WearingAFoundation,
		},
		new AmbientDef
		{
			Line = "The things in your satchel have stopped rattling against each other.",
			When = () => Vigil.Satchel.Count >= 6,
		},
		new AmbientDef
		{
			Line = "Your hands know the work now. You have stopped watching them.",
			When = () => Vigil.HandGathers >= 500,
		},
		new AmbientDef
		{
			Line = "The parish is louder than one keeper should be able to make it.",
			When = () => Vigil.OfferingsCount() >= 20,
		},
		new AmbientDef
		{
			Line = "You have been at the edge of it so long that the edge has moved.",
			When = () => Vigil.Dread >= 0.75,
		},
		new AmbientDef
		{
			Line = "Nothing has come for a while. That is not the same as nothing coming.",
			When = () => Vigil.Dread <= 0.25 && Vigil.PlayedSeconds > 600.0,
		},
		new AmbientDef
		{
			Line = "Something you are carrying is warm, and it was not a moment ago.",
			When = () => Vigil.BestRelicGrade >= (int)Grade.Hallowed,
		},
		new AmbientDef
		{
			Line = "The deepest of them has stopped needing to be lit.",
			When = () => Vigil.Owned[Content.RiteCount - 1] > 0,
		},
		new AmbientDef
		{
			Line = "You have done this before. The ledger is in your handwriting throughout.",
			When = () => Vigil.Communions >= 1,
		},
		new AmbientDef
		{
			Line = "Whatever you rendered down, some of it is still in the air.",
			When = () => Vigil.RelicsRendered >= 5,
		},
		new AmbientDef
		{
			Line = "One of the wards has gone out. You did not see which.",
			When = () => Vigil.Wards >= 1,
		},
	};

	/// <summary>Said to a keeper who has never gone looking. The one nudge in the game, and it
	/// is phrased as an observation rather than an instruction, because the parish does not
	/// give advice.</summary>
	public const string Beckon = "The dark keeps its distance. It will not come to you unless you go to it.";

	/// <summary>Progress toward a count, clamped, for a mark's bar.</summary>
	private static double Along(double have, double need)
		=> need <= 0.0 ? 0.0 : Math.Clamp(have / need, 0.0, 1.0);

	public static readonly MarkDef[] Marks =
	{
		new MarkDef { Name = "First Light", Blurb = "Light a Grave Lantern.", Earned = () => Vigil.Owned[0] >= 1 },
		new MarkDef { Name = "Full Choir", Blurb = "Keep twelve of anything.", Earned = () => Vigil.AnyOwnedAtLeast(12), Toward = () => Along(Vigil.MostOwned(), 12) },
		new MarkDef { Name = "Deep Ledger", Blurb = "Gather a million ichor, all told.", Earned = () => Vigil.LifetimeIchor >= 1e6, Toward = () => Along(Vigil.LifetimeIchor, 1e6) },
		new MarkDef { Name = "Steady Nerve", Blurb = "Hold above three quarters dread for a minute.", Earned = () => Vigil.HighDreadSeconds >= 60.0, Toward = () => Along(Vigil.HighDreadSeconds, 60.0) },
		new MarkDef { Name = "Warded", Blurb = "Turn a visitation away with a ward.", Earned = () => Vigil.WardsRaised >= 1 },
		new MarkDef { Name = "Bereaved", Blurb = "Lose something to the dark.", Earned = () => Vigil.TimesTaken >= 1 },
		new MarkDef { Name = "Communed", Blurb = "Give the parish back and take a sigil.", Earned = () => Vigil.Communions >= 1 },
		new MarkDef { Name = "Marked", Blurb = "Take ten sigils, all told.", Earned = () => Vigil.SigilsEarned >= 10, Toward = () => Along(Vigil.SigilsEarned, 10) },
		new MarkDef { Name = "The Whole Nave", Blurb = "Own one of every rite.", Earned = () => Vigil.OwnsOneOfEach(), Toward = () => Along(Vigil.RitesHeld(), RiteCount) },
		new MarkDef { Name = "Mouth to Mouth", Blurb = "Open a Hollow Mouth.", Earned = () => Vigil.Owned[7] >= 1 },
		new MarkDef { Name = "Not Alone", Blurb = "Keep vigil beside another keeper.", NeedsCongregation = true, Earned = () => Vigil.SharedVigilSeconds >= 30.0, Toward = () => Along(Vigil.SharedVigilSeconds, 30.0) },
		new MarkDef { Name = "Answered", Blurb = "Ring the bell into a communion.", NeedsCongregation = true, Earned = () => Vigil.CommunionSurges >= 1 },
		new MarkDef { Name = "Deepened", Blurb = "Carry one boon as far as it goes.", Earned = AnyBoonMaxed },
		new MarkDef { Name = "Named", Blurb = "Turn something away by knowing what it wanted.", Earned = () => Vigil.VisitorsAnswered >= 1 },
		new MarkDef { Name = "Well Read", Blurb = "Turn away twenty of them.", Earned = () => Vigil.VisitorsAnswered >= 20, Toward = () => Along(Vigil.VisitorsAnswered, 20) },

		// ── Appended, and they must stay appended ────────────────────────────────────
		// Which marks a keeper has earned is stored as flags indexed by POSITION, exactly as
		// the offerings are, so reordering this table hands somebody a record of things they
		// never did. New marks go on the end. There is a check in the harness that holds the
		// order of everything above this line.
		new MarkDef { Name = "Turned Up", Blurb = "Dig something out of the parish.", Earned = () => Vigil.RelicsFound >= 1 },
		new MarkDef { Name = "Grave Goods", Blurb = "Turn up fifty of them.", Earned = () => Vigil.RelicsFound >= 50, Toward = () => Along(Vigil.RelicsFound, 50) },
		new MarkDef { Name = "Hollowed Out", Blurb = "Find something that should not have been down there.", Earned = () => Vigil.BestRelicGrade >= (int)Grade.Hollowed, Toward = () => Along(Vigil.BestRelicGrade + 1, (int)Grade.Hollowed + 1) },
		new MarkDef { Name = "Both Hands and One More", Blurb = "Wear three relics at once.", Earned = () => Vigil.RelicsWorn() >= 3, Toward = () => Along(Vigil.RelicsWorn(), Relics.Slots) },
		new MarkDef { Name = "Rendered Down", Blurb = "Melt twenty finds back into ichor.", Earned = () => Vigil.RelicsRendered >= 20, Toward = () => Along(Vigil.RelicsRendered, 20) },
		new MarkDef { Name = "Underwritten", Blurb = "Wear something that stands where nothing was built.", Earned = Vigil.WearingAFoundation },
		new MarkDef { Name = "Consecrant", Blurb = "Give a vigil to one rite and mean it.", Earned = () => Vigil.Consecrations >= 1 },
		new MarkDef { Name = "Well Provisioned", Blurb = "Have forty offerings standing at once.", Earned = () => Vigil.OfferingsCount() >= 40, Toward = () => Along(Vigil.OfferingsCount(), 40) },
	};
}
