using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The two pieces of screen that are always there: the bar across the top that says what
/// the vigil is worth, and the nave under it - the sigil you gather by hand from, the ward,
/// and the bell.
/// </summary>
/// <remarks>
/// Both are laid out inside the screen MINUS the ledger's width, anchored to the edges
/// rather than placed at absolute pixels, so the whole HUD reflows when the window is
/// resized without anything re-measuring or rebuilding.
/// </remarks>
public sealed class Hud
{
	/// <summary>Width the ledger reserves on the right. Everything here stops short of it.</summary>
	/// <summary>Width of the ledger. Widened from 620: every row is four columns of one-line
	/// text, and at 620 the longest of them ran out of room and clipped mid-word. The parish has
	/// the space to give - it is mostly empty floor.</summary>
	public const float LedgerWidth = 760.0f;
	public const float BarHeight = 104.0f;

	private const float kSigilSize = 268.0f;

	private Entity _bar;
	private Entity _nave;

	private Entity _title;
	private Entity _keeper;
	private Entity _ichor;
	private Entity _rate;
	private Entity _multiplier;
	private Entity _dreadTrack;
	private Entity _dreadLabel;
	private Entity _dreadCaption;
	private Entity _sigilCount;

	private Entity _sigil;
	private Entity _sigilHint;
	private Entity _handValue;
	private Entity _fervourBar;
	private Entity _foundPanel;
	private Entity _foundArt;
	private Entity _foundGrade;
	private Entity _foundName;
	private Entity _foundPowers;

	/// <summary>Seconds the found banner has left. Long enough to read a name and three
	/// powers, short enough that a keeper turning things up steadily is not reading a wall.</summary>
	private float _foundFor;

	private Entity _visPanel;
	private Entity _visName;
	private Entity _visLine;
	private Entity _visClock;
	/// <summary>The four answers, indexed by <see cref="Answer"/> minus one - None has no
	/// button, because not answering is what happens when you touch nothing.</summary>
	private readonly Button[] _answers = new Button[4];

	private Button _stoke;
	private Button _ward;
	private Button _bell;

	/// <summary>Seconds of punch left on the sigil. Drives its size and its shader, and is the
	/// only reason a click feels like anything.</summary>
	private float _punch;

	public Entity SigilElement => _sigil;

	/// <summary>True on the frame the keeper gathered by hand - by clicking the sigil, by
	/// activating it with a pad, or with the space bar.</summary>
	public bool GatheredThisFrame { get; private set; }

	public bool WardPressed { get; private set; }
	public bool BellPressed { get; private set; }
	public bool StokePressed { get; private set; }

	/// <summary>The answer the keeper reached for this frame, or <see cref="Answer.None"/>.</summary>
	public Answer AnswerGiven { get; private set; }

	/// <summary>Where the last gather landed, in SCREEN pixels: the pointer when the sigil was
	/// clicked, and the sigil itself when it was the space bar. Feedback is thrown from here,
	/// so it appears where the keeper actually struck.</summary>
	public Vector2 StrikePoint { get; private set; }

	/// <summary>
	/// Put a find on screen, properly.
	/// </summary>
	/// <remarks>
	/// The whole reason relics exist is to make hand-gathering worth doing at hour ten, and a
	/// reward that arrives silently in a panel the keeper may not have open is not a reward
	/// they feel. Playtested, a keeper could not tell they had found anything without watching
	/// the satchel - so the find now takes the same band the visitation uses, with its own
	/// drawing, its grade, its name and what it does.
	/// </remarks>
	public void ShowFound(Relic relic)
	{
		if (!relic.Exists)
		{
			return;
		}
		_found = relic;
		// Better things stay up longer. A Leavings is a glance; a Hollowed thing is an event.
		_foundFor = 3.5f + (int)relic.Grade * 0.9f;
	}

	private Relic _found;

	/// <summary>
	/// Count the banner down, and yield the band to anything walking.
	/// </summary>
	/// <remarks>
	/// A visitation always wins the slot: for the nine seconds something is coming, that is the
	/// only thing worth looking at, and two panels fighting over one place on screen is worse
	/// than either of them missing.
	/// </remarks>
	private void UpdateFound(float deltaTime)
	{
		_foundFor = MathF.Max(0.0f, _foundFor - deltaTime);
		bool show = _foundFor > 0.0f && _found.Exists && !Vigil.Approaching;
		_foundPanel.SetActive(show);
		if (!show)
		{
			return;
		}

		Ui.SetMaterialParams(_foundArt, new Vector4(Time.UnscaledTime, Relics.ArtSeed(_found),
			(float)(int)_found.Grade, 1.0f));
		Ui.SetMaterialColors(_foundArt, Palette.Fade(Palette.Ichor, Relics.PackedPowerMask(_found)),
			Palette.Dread);

		// The grade said outright and in full, not implied by a shade. A keeper should not have
		// to compare two rows to work out whether what they just dug up was any good.
		Ui.SetText(_foundGrade, Relics.GradeName(_found.Grade).ToUpperInvariant() + Pips(_found.Grade));
		Ui.SetTextColor(_foundGrade, Palette.Mix(Palette.TextDim, Palette.Ichor, GradeWeight(_found.Grade)));
		Ui.SetText(_foundName, Relics.NameOf(_found));

		string powers = "";
		for (int i = 0; i < Relics.PowerCount(_found.Grade); i++)
		{
			powers += (powers.Length > 0 ? "   " : "") +
				Relics.DescribeOn(_found, i);
		}
		Ui.SetText(_foundPowers, powers);
	}

	/// <summary>Grade as a run of marks. ASCII only - the font pipeline bakes no other glyph
	/// set, which is the same reason Numbers keeps to two-character suffixes.</summary>
	public static string Pips(Grade grade)
	{
		string pips = "  ";
		for (int i = 0; i <= (int)grade; i++)
		{
			pips += "+";
		}
		return pips;
	}

	/// <summary>0 to 1 across the ladder, for tinting.</summary>
	public static float GradeWeight(Grade grade) => 0.3f + (int)grade / 4.0f * 0.7f;

	/// <summary>
	/// Show whatever is walking, and read the answer.
	/// </summary>
	/// <remarks>
	/// Every answer stays on screen for the whole encounter, including the ones this keeper
	/// cannot pay for - they go dead rather than disappearing. A row that changes shape as
	/// your purse changes is a row you cannot learn, and learning which verb belongs to which
	/// visitor is the only progression this game keeps in the player rather than in the save.
	/// Seeing that a ward WOULD have answered is how the next one goes better.
	/// </remarks>
	private void UpdateVisitation()
	{
		bool walking = Vigil.Approaching;
		_visPanel.SetActive(walking);
		for (int i = 0; i < _answers.Length; i++)
		{
			_answers[i].SetActive(walking);
		}
		if (!walking)
		{
			return;
		}

		RiteDef visitor = Content.Rites[Vigil.ApproachRite];
		Ui.SetText(_visName, visitor.VisitorName);
		// The line the parish actually said, not the canonical one - otherwise the panel and the
		// whisper describe the same visitor in two different sentences.
		Ui.SetText(_visLine, Vigil.ApproachLine.Length > 0 ? Vigil.ApproachLine : visitor.Approach);
		// What is at stake, said while there is still time to do something about it. A visitation
		// that lands takes the best thing loose in the satchel, and a keeper who only finds that
		// out afterwards has been punished for a rule nobody told them. Said HERE rather than in
		// the feed because this is the panel they are already looking at, and the decision - put
		// it on, or risk it - has a clock running on it.
		// Asks the simulation which one is exposed rather than working it out again here, so the
		// name on the panel is always the name of the thing that would actually go.
		int exposedIndex = Vigil.MostExposed();
		string exposed = exposedIndex >= 0
			? "   -   " + Relics.NameOf(Vigil.Satchel[exposedIndex]) + " is loose in your satchel"
			: "";
		Ui.SetText(_visClock, "IT ARRIVES IN " + MathF.Ceiling((float)Vigil.ApproachSeconds).ToString("0")
			+ "s" + exposed);
		// The clock reddens as it runs out, so the pressure is felt rather than read.
		float urgency = 1.0f - (float)(Vigil.ApproachSeconds / Math.Max(0.001, Vigil.ApproachTotal));
		Ui.SetTextColor(_visClock, Palette.Mix(Palette.TextFaint, Palette.DreadText, urgency));

		bool canWard = Vigil.Wards > 0;
		bool canOffer = Vigil.Ichor >= Vigil.OfferCost;
		_answers[0].SetLabel(canWard ? "WARD IT  " + Vigil.Wards : "NO WARD");
		_answers[1].SetLabel(canOffer ? "OFFER " + Numbers.Short(Vigil.OfferCost) : "CANNOT OFFER");
		_answers[0].SetEnabled(canWard);
		_answers[1].SetEnabled(canOffer);
		_answers[2].SetEnabled(true);
		_answers[3].SetEnabled(true);

		for (int i = 0; i < _answers.Length; i++)
		{
			bool live = i switch { 0 => canWard, 1 => canOffer, _ => true };
			_answers[i].Style(live, Palette.Mix(Palette.Row, Palette.Dread, 0.45f), Palette.Row, Palette.PanelDeep);
			if (live && _answers[i].Activated)
			{
				AnswerGiven = (Answer)(i + 1);
			}
		}
	}

	/// <summary>Bind the authored chrome. Every element here lives in the scene, so this only
	/// looks things up - laying the bar out is a job for the editor, not for a rebuild.</summary>
	public void Bind()
	{
		_bar = Scene.Find("HudBar");
		_title = Scene.Find("HudTitle");
		_keeper = Scene.Find("HudKeeper");
		_ichor = Scene.Find("HudIchor");
		_rate = Scene.Find("HudRate");
		_multiplier = Scene.Find("HudMultiplier");
		_dreadCaption = Scene.Find("HudDreadCaption");
		_dreadTrack = Scene.Find("HudDreadBar");
		_dreadLabel = Scene.Find("HudDreadValue");
		_sigilCount = Scene.Find("HudSigils");

		_nave = Scene.Find("HudNave");
		_sigil = Scene.Find("NaveSigil");
		_sigilHint = Scene.Find("NaveHint");
		_handValue = Scene.Find("NaveHandValue");
		_fervourBar = Scene.Find("NaveFervourBar");
		_foundPanel = Scene.Find("FoundPanel");
		_foundArt = Scene.Find("FoundArt");
		_foundGrade = Scene.Find("FoundGrade");
		_foundName = Scene.Find("FoundName");
		_foundPowers = Scene.Find("FoundPowers");

		_visPanel = Scene.Find("VisitationPanel");
		_visName = Scene.Find("VisitationName");
		_visLine = Scene.Find("VisitationLine");
		_visClock = Scene.Find("VisitationClock");
		_answers[0] = Button.Find("AnswerWard");
		_answers[1] = Button.Find("AnswerOffer");
		_answers[2] = Button.Find("AnswerBell");
		_answers[3] = Button.Find("AnswerStill");

		_stoke = Button.Find("NaveStoke");
		_ward = Button.Find("NaveWard");
		_bell = Button.Find("NaveBell");
	}

	/// <summary>Read input, animate, and write every live number. Called once a frame.</summary>
	public void Update(float deltaTime, bool connected, int keeperCount)
	{
		GatheredThisFrame = false;
		WardPressed = false;
		BellPressed = false;
		StokePressed = false;
		AnswerGiven = Answer.None;

		// ── Input ────────────────────────────────────────────────────────────────────
		// WasActivated covers the click, Enter/Space-while-focused and the pad button, and
		// consumes the key so it cannot also reach the shortcut below on the same frame.
		if (_sigil.IsValid && Ui.WasActivated(_sigil))
		{
			GatheredThisFrame = true;
			// The pointer if it is over the sigil, the sigil's middle if the activation came
			// from a key or a pad - either way, a place the player was looking at.
			Vector4 rect = Ui.GetRect(_sigil);
			StrikePoint = Ui.IsHovered(_sigil)
				? Input.MousePosition
				: new Vector2(rect.X + rect.Z * 0.5f, rect.Y + rect.W * 0.5f);
		}
		else if (Input.IsKeyPressed(Key.Space) && !Ui.HasFocus)
		{
			Vector4 rect = Ui.GetRect(_sigil);
			StrikePoint = new Vector2(rect.X + rect.Z * 0.5f, rect.Y + rect.W * 0.5f);
			// Space works with nothing focused, which is the state the game spends most of its
			// time in. Gated on HasFocus so it cannot double-fire with the line above, and so a
			// keeper typing a name into the threshold screen is not also gathering.
			GatheredThisFrame = true;
		}

		if (_stoke.Activated)
		{
			StokePressed = true;
		}
		if (_ward.Activated)
		{
			WardPressed = true;
		}
		// Only reported. Whether the rope actually moves - and the cooldown it starts - is the
		// vigil's to decide, the same as the stoke button beside it; the HUD arming its own
		// timer meant the one rule governing the bell was a float on a widget.
		if (_bell.Activated)
		{
			BellPressed = true;
		}

		if (GatheredThisFrame)
		{
			_punch = 1.0f;
		}

		// ── Animation ────────────────────────────────────────────────────────────────
		_punch = MathF.Max(0.0f, _punch - deltaTime * 3.4f);

		float dread = (float)Vigil.Dread;
		bool hovered = _sigil.IsValid && Ui.IsHovered(_sigil);
		// A quick snap out and a slow settle back: the size curve is the whole feel of the
		// click, so it is eased rather than linear.
		float eased = _punch * _punch;
		float scale = 1.0f + eased * 0.10f + (hovered ? 0.02f : 0.0f);
		float size = kSigilSize * scale;
		if (_sigil.IsValid)
		{
			Ui.SetRect(_sigil, 0.0f, -60.0f, size, size);
			Ui.SetMaterialParams(_sigil, new Vector4(Time.UnscaledTime, dread, eased,
				Vigil.SurgeSeconds > 0.0 ? 1.0f : 0.0f));
			Ui.SetMaterialColors(_sigil, Palette.Ichor, Palette.Dread);
		}

		// ── Readouts ─────────────────────────────────────────────────────────────────
		Ui.SetText(_ichor, Numbers.Short(Vigil.Ichor));
		Ui.SetText(_rate, Numbers.Rate(Vigil.Rate));
		Ui.SetText(_keeper, Vigil.KeeperName + (connected ? "  -  keeping vigil with " + Math.Max(0, keeperCount - 1) : "  -  alone"));
		// ── Fervour ──────────────────────────────────────────────────────────────────
		// Drawn at all, which it never used to be. It is worth up to x1.60 - second only to
		// dread - and it empties in twelve seconds, so a player who could not see it had no
		// way to learn that gathering by hand does anything beyond the ichor it hands over,
		// and no way to read the one boon sold against it.
		float fervour = (float)Vigil.Fervour;
		Ui.SetProgress(_fervourBar, fervour);
		// Dim and cold when it is empty, full ichor when it is not: the bar has to read as
		// something you LIT rather than as a gauge that happens to be low.
		_fervourBar.Component("UI Progress Bar").SetVector4("fill_color",
			Palette.Mix(Palette.IchorDim, Palette.Ichor, MathF.Min(1.0f, fervour * 1.4f)));

		Ui.SetText(_handValue, "+" + Numbers.Short(Vigil.HandGain) + " by hand" +
			(fervour > 0.005f ? "   FERVOUR " + Numbers.Mult(Vigil.FervourMultiplier) : ""));

		string mult = Numbers.Mult(Vigil.GlobalMultiplier);
		Ui.SetText(_multiplier, Vigil.SurgeSeconds > 0.0
			? mult + "  SURGE " + Numbers.Duration(Vigil.SurgeSeconds)
			: mult);
		Ui.SetTextColor(_multiplier, Vigil.SurgeSeconds > 0.0 ? Palette.Ichor : Palette.Sigil);

		// What the bargain is paying, which is the whole reason anyone stands this close to it.
		// The meter said how full it was and how long was left, but never what the risk BOUGHT -
		// so the game's central decision had two of its four inputs missing, and "ride the top
		// or play it safe" was a judgement the interface gave the player no way to make.
		Ui.SetText(_dreadCaption, "DREAD   PAYS " + Numbers.Mult(Vigil.DreadMultiplier));
		// Faint when the bargain is worth nothing, full dread when it is worth a great deal:
		// the caption brightens as the offer gets good, which is the same thing that makes it
		// dangerous.
		Ui.SetTextColor(_dreadCaption, Palette.Mix(Palette.TextFaint, Palette.DreadText, dread));

		Ui.SetProgress(_dreadTrack, dread);
		// The meter says how close, and now also how long. The whole game is a clock the
		// keeper is choosing to stand next to; hiding the clock made riding the top of the
		// meter a guess rather than a decision. A parish too small to reach the top says so
		// instead of quoting an infinity.
		double due = Vigil.SecondsToVisitation;
		Ui.SetText(_dreadLabel, double.IsInfinity(due)
			? Numbers.Percent(dread) + "   holding"
			: Numbers.Percent(dread) + "   " + Numbers.Duration(due));
		// The meter goes from rust to something brighter as it fills, so the last quarter
		// reads as urgent without a second widget to say so.
		_dreadTrack.Component("UI Progress Bar").SetVector4("fill_color",
			Palette.Mix(Palette.DreadDeep, Palette.DreadHot, MathF.Min(1.0f, dread * 1.15f)));

		Ui.SetText(_sigilCount, Vigil.Sigils + " sigils   " + Vigil.MarksHeld() + "/" + Content.Marks.Length + " marks");

		// ── Buttons ──────────────────────────────────────────────────────────────────
		// Stoking pays now, so the button says what it pays - and the figure is asked of the
		// simulation rather than recomputed here, because this line and Vigil.Stoke were two
		// copies of one formula and had already drifted into being wrong together.
		bool canStoke = Vigil.CanStoke;
		_stoke.SetLabel(Vigil.Dread >= 0.999
			? "IT IS AS CLOSE AS IT GETS"
			: Vigil.StokeCooldown > 0.0
				? "IT IS STILL LISTENING  " + Numbers.Duration(Vigil.StokeCooldown)
				: "STOKE THE DARK  +" + Numbers.Short(Vigil.StokeOffer));
		_stoke.SetEnabled(canStoke);
		_stoke.Style(canStoke, Palette.Mix(Palette.Row, Palette.Dread, 0.55f), Palette.Row, Palette.PanelDeep);

		// Wards are held charges, so the label reports how many are in hand. Insurance the
		// player cannot count is insurance they will not trust.
		bool full = Vigil.Wards >= Vigil.MaxWards;
		bool canWard = Vigil.Ichor >= Vigil.WardCost && !full;
		_ward.SetLabel(full
			? "WARDS  " + Vigil.Wards + "/" + Vigil.MaxWards
			: "SET A WARD  " + Vigil.Wards + "/" + Vigil.MaxWards + "   " + Numbers.Short(Vigil.WardCost));
		_ward.SetEnabled(canWard);
		_ward.Style(canWard, Palette.Mix(Palette.Row, Palette.Dread, 0.45f), Palette.Row, Palette.PanelDeep);

		_bell.SetActive(connected);
		if (connected)
		{
			bool ready = Vigil.CanRing;
			_bell.SetLabel(ready ? "RING THE BELL" : "BELL  " + Numbers.Duration(Vigil.BellCooldown));
			_bell.SetEnabled(ready);
			_bell.Style(ready, Palette.Mix(Palette.Row, Palette.Sigil, 0.40f), Palette.Row, Palette.PanelDeep);
		}

		UpdateFound(deltaTime);
		UpdateVisitation();

		Ui.SetTextColor(_sigilHint, Palette.Mix(Palette.TextFaint, Palette.Ichor, eased));
		Ui.SetTextColor(_title, Palette.Mix(Palette.TextDim, Palette.DreadText, dread * 0.8f));
	}
}
