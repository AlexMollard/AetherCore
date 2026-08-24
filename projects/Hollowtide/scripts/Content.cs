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
	public double Multiplier;
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
			VisitorName = "The Wick-Thin Man", Answer = Answer.Still,
			Approach = "Something thin is walking the lantern line, and stopping at each one.",
		},
		new RiteDef
		{
			Name = "Bone Choir", Art = "project://assets/textures/rites/bone_choir.png", CycleSeconds = 3.5,
			Blurb = "Twelve throats, no air, and they keep perfect time.",
			BaseCost = 110.0, BaseRate = 0.9, Growth = 1.14, DreadRate = 0.0022,
			TakenLine = "The choir drops a voice. The others do not adjust.",
			VisitorName = "The Thirteenth Voice", Answer = Answer.Bell,
			Approach = "A voice joins the choir. It is holding a note none of them started.",
		},
		new RiteDef
		{
			Name = "Weeping Statue", Art = "project://assets/textures/rites/weeping_statue.png", CycleSeconds = 5.0,
			Blurb = "You have never seen it move. It is never where it was.",
			BaseCost = 1300.0, BaseRate = 7.0, Growth = 1.15, DreadRate = 0.0044,
			TakenLine = "A plinth stands empty. The stains lead away from it.",
			VisitorName = "The Unmoved", Answer = Answer.Still,
			Approach = "The statue is facing the other way. You are certain it is watching.",
		},
		new RiteDef
		{
			Name = "Flesh Loom", Art = "project://assets/textures/rites/flesh_loom.png", CycleSeconds = 7.0,
			Blurb = "It asks for very little and it never stops asking.",
			BaseCost = 15000.0, BaseRate = 44.0, Growth = 1.15, DreadRate = 0.0080,
			TakenLine = "The loom is unthreaded. Something wore what it made.",
			VisitorName = "The Unthreaded", Answer = Answer.Offer,
			Approach = "Something is pulling at the weave, and it is hungry rather than cruel.",
		},
		new RiteDef
		{
			Name = "Ossuary Engine", Art = "project://assets/textures/rites/ossuary_engine.png", CycleSeconds = 9.0,
			Blurb = "Built from the parish it drains. It is very efficient.",
			BaseCost = 190000.0, BaseRate = 260.0, Growth = 1.16, DreadRate = 0.0140,
			TakenLine = "An engine seizes. The bones in it were not ours.",
			VisitorName = "The Millwright", Answer = Answer.Offer,
			Approach = "The engine is running faster than you set it. Something is feeding it.",
		},
		new RiteDef
		{
			Name = "Drowned Chapel", Art = "project://assets/textures/rites/drowned_chapel.png", CycleSeconds = 12.0,
			Blurb = "The tide keeps the congregation. The congregation keeps singing.",
			BaseCost = 2600000.0, BaseRate = 1500.0, Growth = 1.16, DreadRate = 0.0240,
			TakenLine = "A chapel slips under. The singing does not stop, only muffles.",
			VisitorName = "The Tide-Sung", Answer = Answer.Bell,
			Approach = "The water in the nave is rising, and the singing is getting louder.",
		},
		new RiteDef
		{
			Name = "Pale Shepherd", Art = "project://assets/textures/rites/pale_shepherd.png", CycleSeconds = 16.0,
			Blurb = "It gathers what wanders. You have agreed not to wander.",
			BaseCost = 42000000.0, BaseRate = 8800.0, Growth = 1.17, DreadRate = 0.0420,
			TakenLine = "A shepherd walks off with its flock. Count yourself.",
			VisitorName = "The Shepherd's Count", Answer = Answer.Ward,
			Approach = "It has begun counting the flock. Do not let it reach you.",
		},
		new RiteDef
		{
			Name = "Hollow Mouth", Art = "project://assets/textures/rites/hollow_mouth.png", CycleSeconds = 22.0,
			Blurb = "It is not a door. Doors are for going back through.",
			BaseCost = 720000000.0, BaseRate = 51000.0, Growth = 1.18, DreadRate = 0.0700,
			TakenLine = "A mouth closes. You are certain it swallowed.",
			VisitorName = "What Came Through", Answer = Answer.Ward,
			Approach = "The mouth is open wider than it opens. Something is using it as a door.",
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

	/// <summary>Three offerings per rite on a fixed ladder, plus the hand-written global
	/// ones. Generating the ladder keeps 24 near-identical entries out of the file and
	/// makes adding a ninth rite a one-line change.</summary>
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
		new BoonDef
		{
			Name = "The Old Bargain", Blurb = "Dread pays better. It always did, for the ones who asked twice.",
			BaseCost = 10.0, MaxLevel = 3, Step = 0.55,
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

	public static readonly MarkDef[] Marks =
	{
		new MarkDef { Name = "First Light", Blurb = "Light a Grave Lantern.", Earned = () => Vigil.Owned[0] >= 1 },
		new MarkDef { Name = "Full Choir", Blurb = "Keep twelve of anything.", Earned = () => Vigil.AnyOwnedAtLeast(12) },
		new MarkDef { Name = "Deep Ledger", Blurb = "Gather a million ichor, all told.", Earned = () => Vigil.LifetimeIchor >= 1e6 },
		new MarkDef { Name = "Steady Nerve", Blurb = "Hold above three quarters dread for a minute.", Earned = () => Vigil.HighDreadSeconds >= 60.0 },
		new MarkDef { Name = "Warded", Blurb = "Turn a visitation away with a ward.", Earned = () => Vigil.WardsRaised >= 1 },
		new MarkDef { Name = "Bereaved", Blurb = "Lose something to the dark.", Earned = () => Vigil.TimesTaken >= 1 },
		new MarkDef { Name = "Communed", Blurb = "Give the parish back and take a sigil.", Earned = () => Vigil.Communions >= 1 },
		new MarkDef { Name = "Marked", Blurb = "Take ten sigils, all told.", Earned = () => Vigil.SigilsEarned >= 10 },
		new MarkDef { Name = "The Whole Nave", Blurb = "Own one of every rite.", Earned = () => Vigil.OwnsOneOfEach() },
		new MarkDef { Name = "Mouth to Mouth", Blurb = "Open a Hollow Mouth.", Earned = () => Vigil.Owned[7] >= 1 },
		new MarkDef { Name = "Not Alone", Blurb = "Keep vigil beside another keeper.", Earned = () => Vigil.SharedVigilSeconds >= 30.0 },
		new MarkDef { Name = "Answered", Blurb = "Ring the bell into a communion.", Earned = () => Vigil.CommunionSurges >= 1 },
		new MarkDef { Name = "Deepened", Blurb = "Carry one boon as far as it goes.", Earned = AnyBoonMaxed },
		new MarkDef { Name = "Named", Blurb = "Turn something away by knowing what it wanted.", Earned = () => Vigil.VisitorsAnswered >= 1 },
		new MarkDef { Name = "Well Read", Blurb = "Turn away twenty of them.", Earned = () => Vigil.VisitorsAnswered >= 20 },
	};
}
