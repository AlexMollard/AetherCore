using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

public enum LedgerTab
{
	Rites,
	Offerings,
	Communion,
	Marks,
	Relics,
	Voices,
}

/// <summary>
/// The right-hand column: everything the keeper can spend on, in four tabs.
/// </summary>
/// <remarks>
/// <para>
/// One pool of row widgets, reused by every tab. A tab does not own rows - it fills them.
/// That is what keeps four quite different lists (buyable rites, one-off offerings, sigil
/// spending, and a read-only record) inside one layout, one scroll model and one set of
/// hit-tests, instead of four panels that each drift.
/// </para>
/// <para>
/// Scrolling moves by whole ITEMS rather than by pixels. A pooled row that is half off the
/// top of a mask is a row whose hit-box you can still click, and an idle game is played by
/// clicking rows repeatedly - so a partial row at the edge is a mis-buy waiting to happen.
/// </para>
/// </remarks>
public sealed class Ledger
{
	private const float kWidth = Hud.LedgerWidth;
	/// <summary>Gutter inside the panel. Must match the LedgerViewport inset authored in the
	/// scene: the row pool is sized from it, and 4px of disagreement puts every row past the
	/// mask.</summary>
	private const float kPad = 20.0f;

	/// <summary>Left edge of a row's text, clear of its icon.</summary>
	private const float kTextLeft = 68.0f;

	/// <summary>Gap kept between the text column and the figures column.</summary>
	private const float kColumnGap = 14.0f;

	/// <summary>
	/// The two column widths, derived rather than written down.
	/// </summary>
	/// <remarks>
	/// They were hardcoded at 320 and 366 against a figures column inset from the right, and
	/// the two overlapped by seventy pixels - a row's description ran underneath its own price.
	/// Raising the type made it worse, because the inset scaled with the type while the text
	/// widths did not, walking the price column leftward into the words. Derived from the panel
	/// there is nothing left to fall out of step: the figures take a fixed column and the text
	/// takes whatever is left.
	/// </remarks>
	private static float FiguresWidth => Typography.Box(150.0f);

	private static float TextWidth => kWidth - kPad * 2.0f - FiguresWidth - kTextLeft - kColumnGap;

	private static float FiguresLeft => kWidth - kPad * 2.0f - FiguresWidth;
	/// <summary>Scaled with the type on it. A row is two lines and a rarity bar, and raising the
	/// faces without raising the row put the second line straight through the bar.</summary>
	private static readonly float kRowHeight = Typography.Box(64.0f);
	private const float kRowGap = 6.0f;
	private const float kListTop = 150.0f;
	private const int kRowPool = 14;

	private struct Row
	{
		public Button Box;
		public Entity Title;
		public Entity Sub;
		public Entity Cost;
		public Entity Note;
		/// <summary>A thin fill along the bottom of the row showing how far through its
		/// working the rite is. The row stops being a price tag and becomes a machine.</summary>
		public Entity Progress;
		/// <summary>The relic's own drawing, on the one tab that has anything to draw. Created
		/// with the pool rather than per relic: there is no upper bound on how many relics
		/// exist, but there is a hard bound on how many rows can be on screen.</summary>
		public Entity Icon;
	}

	private Entity _panel;
	private Entity _viewport;
	private readonly Button[] _tabs = new Button[5];
	private readonly Button[] _amounts = new Button[4];

	/// <summary>
	/// Fills the three hands with the best three relics held.
	/// </summary>
	/// <remarks>
	/// Every find meant opening the satchel and comparing rows by hand, and since a better grade
	/// is simply better there was no judgement in it - only bookkeeping the game would not do.
	/// Shares the strip the buy-amount buttons use, which is empty on every tab but the rites.
	/// </remarks>
	private Button _wearBest;

	/// <summary>
	/// The panel that says what a relic actually is.
	/// </summary>
	/// <remarks>
	/// A row has space for a name, a grade and one line of effects, and a relic now carries up
	/// to five of them plus a lender's worth of structures - so the row had become a summary of
	/// something the keeper could not read in full anywhere. This is that full reading: the
	/// drawing at a size where its features can be told apart, every power on its own line, and
	/// what the thing is worth if it is rendered down.
	/// </remarks>
	private Entity _inspect;
	private Entity _inspectArt;
	private Entity _inspectName;
	private Entity _inspectGrade;
	private Entity _inspectFlavour;
	private Entity _inspectFoot;
	private readonly Entity[] _inspectPowers = new Entity[5];

	/// <summary>Set while dressing a relic row the pointer is over. Read after the rows are
	/// filled, because which row is hovered is only known once they have all been placed.</summary>
	private Relic _inspecting;
	private bool _inspectingWorn;
	private double _inspectRenders;
	private readonly Row[] _rows = new Row[kRowPool];

	/// <summary>Tab labels, kept here because the tab now has a mark appended when it has
	/// something waiting, and the scene's authored label is no longer the whole story.</summary>
	private static readonly string[] s_tabNames =
		{ "RITES", "OFFERINGS", "COMMUNION", "MARKS", "RELICS", "VOICES" };

	private static readonly int[] s_amounts = { 1, 10, 100, -1 };
	private static readonly string[] s_amountLabels = { "x1", "x10", "x100", "MAX" };

	private LedgerTab _tab = LedgerTab.Rites;
	private int _amountIndex;
	private int _scroll;

	/// <summary>What the transcript's line count was last frame, so the panel can tell how far
	/// the list moved under a reader who is scrolled back through it.</summary>
	private long _seenSaid;
	private int _itemCount;
	private int _visibleRows = kRowPool;

	public LedgerTab Tab => _tab;

	/// <summary>How many copies a buy button buys: 1, 10, 100, or as many as the purse covers.</summary>
	public int BuyAmount => s_amounts[_amountIndex];

	/// <summary>
	/// Bind the authored chrome, then build the row pool.
	/// </summary>
	/// <remarks>
	/// The split the scene draws is the split that matters: the panel, its tabs and its
	/// buy-amount buttons are fixed furniture and live in the editor, while the ROWS are a
	/// function of the content tables - eight rites, thirty-three offerings, twelve marks -
	/// and are re-filled per tab. Authoring fourteen identical rows in a scene file would be
	/// fourteen copies of one layout to keep in step by hand.
	/// </remarks>
	public void Bind()
	{
		_panel = Scene.Find("LedgerPanel");
		_viewport = Scene.Find("LedgerViewport");

		for (int i = 0; i < _tabs.Length; i++)
		{
			_tabs[i] = Button.Find("LedgerTab" + i);
		}
		_wearBest = Button.Find("LedgerWearBest");
		_inspect = Scene.Find("RelicInspect");
		// Authored active so it can be seen while editing the scene; hidden the moment the game
		// owns it, so it cannot show an empty panel on the frames before the first update.
		if (_inspect.IsValid)
		{
			_inspect.SetActive(false);
		}
		_inspectArt = Scene.Find("InspectArt");
		_inspectName = Scene.Find("InspectName");
		_inspectGrade = Scene.Find("InspectGrade");
		_inspectFlavour = Scene.Find("InspectFlavour");
		_inspectFoot = Scene.Find("InspectFoot");
		for (int i = 0; i < _inspectPowers.Length; i++)
		{
			_inspectPowers[i] = Scene.Find("InspectPower" + i);
		}
		for (int i = 0; i < _amounts.Length; i++)
		{
			_amounts[i] = Button.Find("LedgerAmount" + i);
		}

		float rowWidth = kWidth - kPad * 2.0f;
		for (int i = 0; i < kRowPool; i++)
		{
			Row row = default;
			row.Box = UiKit.MakeButton(_viewport, "", 0.0f, i * (kRowHeight + kRowGap), rowWidth, kRowHeight);
			// The composed button's own centred label is unused: a ledger row has four columns,
			// so they are placed individually and the pooled label is emptied.
			Ui.SetText(row.Box.Label, "");
			row.Title = UiKit.Text(row.Box.Root, "", kTextLeft, Typography.Box(6.0f), TextWidth,
				Typography.Box(24.0f), Typography.Face(17.0f), Palette.TextBright);
			row.Sub = UiKit.Text(row.Box.Root, "", kTextLeft, Typography.Box(32.0f), TextWidth,
				Typography.Box(23.0f), Typography.Face(15.0f), Palette.TextFaint,
				UiHAlign.Left, Palette.Body);
			row.Cost = UiKit.Text(row.Box.Root, "", FiguresLeft,
				Typography.Box(6.0f), FiguresWidth, Typography.Box(24.0f), Typography.Face(17.0f),
				Palette.Ichor, UiHAlign.Right);
			row.Note = UiKit.Text(row.Box.Root, "", FiguresLeft,
				Typography.Box(32.0f), FiguresWidth, Typography.Box(23.0f), Typography.Face(15.0f),
				Palette.TextFaint, UiHAlign.Right);
			row.Progress = UiKit.Image(row.Box.Root, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f, Palette.IchorDim);
			// Ink, not white: ui_relic computes its own colour and takes only the alpha from the
			// element, so on any frame before the material resolves the plain image draws
			// instead - and a white one flashes as a solid block. The same trap the rites hit.
			row.Icon = UiKit.Image(row.Box.Root, 8.0f, 7.0f, 52.0f, 52.0f, Palette.Ink);
			Ui.SetMaterial(row.Icon, "ui_relic");
			row.Icon.SetActive(false);
			_rows[i] = row;
		}
	}

	/// <summary>Read the panel, act on what was pressed, and repaint every row.</summary>
	public void Update()
	{
		HandleTabs();
		HandleScroll();
		Fill();
	}

	private void HandleTabs()
	{
		for (int i = 0; i < _tabs.Length; i++)
		{
			bool active = (int)_tab == i;
			// A mark on any tab with something worth opening it for. Five tabs and no signal
			// meant a keeper had to poll all of them to find out whether the parish was asking
			// for anything - and offerings are permanent doublings, easily missed for an hour.
			bool waiting = Waiting((LedgerTab)i);
			_tabs[i].SetLabel(s_tabNames[i] + (waiting ? " +" : ""));
			_tabs[i].SetColour(active ? Palette.RowHot : Palette.Row);
			_tabs[i].SetLabelColour(active ? Palette.Ichor : waiting ? Palette.Sigil : Palette.TextDim);
			if (_tabs[i].Activated && !active)
			{
				_tab = (LedgerTab)i;
				_scroll = 0;
			}
		}

		bool showBest = _tab == LedgerTab.Relics;
		_wearBest.SetActive(showBest);
		if (showBest)
		{
			// Lit only when it would actually change something, so it never invites a click that
			// does nothing - and says so plainly when the hand is already the best it can be.
			bool worthIt = Vigil.WouldWearBestChange();
			_wearBest.SetEnabled(worthIt);
			_wearBest.SetColour(worthIt ? Palette.Row : Palette.PanelDeep);
			_wearBest.SetLabel(worthIt ? "WEAR THE BEST THREE" : "WEARING THE BEST THREE");
			_wearBest.SetLabelColour(worthIt ? Palette.Ichor : Palette.TextFaint);
			if (_wearBest.Activated && worthIt)
			{
				Vigil.WearBest();
			}
		}

		bool showAmounts = _tab == LedgerTab.Rites;
		for (int i = 0; i < _amounts.Length; i++)
		{
			_amounts[i].SetActive(showAmounts);
			if (!showAmounts)
			{
				continue;
			}
			bool active = _amountIndex == i;
			_amounts[i].SetColour(active ? Palette.RowHot : Palette.Row);
			_amounts[i].SetLabelColour(active ? Palette.Ichor : Palette.TextDim);
			if (_amounts[i].Activated)
			{
				_amountIndex = i;
			}
		}

		// Tab cycles the tabs, which is the shortcut every panel like this has. Skipped while
		// something owns the keyboard so it cannot fight a text field.
		if (Input.IsKeyPressed(Key.Tab) && !Ui.HasFocus)
		{
			_tab = (LedgerTab)(((int)_tab + 1) % _tabs.Length);
			_scroll = 0;
		}
	}

	/// <summary>
	/// Is there anything on this tab worth opening it for?
	/// </summary>
	/// <remarks>
	/// Affordability, not mere existence - a tab full of things the keeper cannot buy is not
	/// news. Marks never signal: nothing there is bought, and a record that nags is just a
	/// record you stop reading.
	/// </remarks>
	private static bool Waiting(LedgerTab tab)
	{
		switch (tab)
		{
			case LedgerTab.Rites:
				for (int rite = 0; rite < Content.RiteCount; rite++)
				{
					if (Vigil.CostOf(rite, Vigil.Owned[rite]) <= Vigil.Ichor)
					{
						return true;
					}
				}
				return false;

			case LedgerTab.Offerings:
				for (int i = 0; i < Content.Offerings.Length; i++)
				{
					if (Vigil.OfferingAvailable(i) && Content.Offerings[i].Cost <= Vigil.Ichor)
					{
						return true;
					}
				}
				return false;

			case LedgerTab.Communion:
				for (int i = 0; i < Content.Boons.Length; i++)
				{
					int cost = Vigil.BoonCost(i);
					if (cost > 0 && Vigil.Sigils >= cost)
					{
						return true;
					}
				}
				for (int rite = 0; rite < Content.RiteCount; rite++)
				{
					if (!Vigil.Overseers[rite] && Vigil.Sigils >= Vigil.OverseerCost(rite))
					{
						return true;
					}
				}
				return false;

			case LedgerTab.Voices:
				// Never marked. A transcript that nags is a transcript you stop opening, and
				// the feed has already said everything in it once, on screen, in its own time.
				return false;

			case LedgerTab.Relics:
				// Something carried that beats something worn, or a free hand to fill.
				for (int slot = 0; slot < Relics.Slots; slot++)
				{
					if (!Vigil.Worn[slot].Exists && Vigil.Satchel.Count > 0)
					{
						return true;
					}
				}
				foreach (Relic carried in Vigil.Satchel)
				{
					foreach (Relic worn in Vigil.Worn)
					{
						if (worn.Exists && carried.Grade > worn.Grade)
						{
							return true;
						}
					}
				}
				return false;

			default:
				return false;
		}
	}

	/// <summary>
	/// Show what the pointer is on, in full.
	/// </summary>
	/// <remarks>
	/// Runs after the rows are dressed, because which row is hovered is only settled once they
	/// have all been placed. Hover rather than a click: inspecting is not a decision, and making
	/// somebody click to read - and click again to stop reading - turns browsing a satchel into
	/// a chore. Clicking a row already means WEAR, which is the right thing for a click to mean.
	/// </remarks>
	private void DrawInspector()
	{
		if (!_inspect.IsValid)
		{
			return;
		}
		bool show = _tab == LedgerTab.Relics && _inspecting.Exists;
		_inspect.SetActive(show);
		if (!show)
		{
			return;
		}

		Relic relic = _inspecting;
		Ui.SetMaterialParams(_inspectArt, new Vector4(Time.UnscaledTime, Relics.ArtSeed(relic),
			(float)(int)relic.Grade, 1.0f));
		// The same power mask the row's small drawing gets, so the big one is the same object
		// rather than a second illustration that might disagree with it.
		Ui.SetMaterialColors(_inspectArt, Palette.Fade(Palette.Ichor, Relics.PackedPowerMask(relic)),
			Palette.Dread);

		Ui.SetText(_inspectName, Relics.NameOf(relic));
		Ui.SetTextColor(_inspectName, Palette.Mix(Palette.TextDim, Palette.Ichor,
			Hud.GradeWeight(relic.Grade)));
		Ui.SetText(_inspectGrade, Relics.GradeName(relic.Grade).ToUpperInvariant() + Hud.Pips(relic.Grade));
		Ui.SetTextColor(_inspectGrade, Palette.Mix(Palette.TextFaint, Palette.Ichor,
			Hud.GradeWeight(relic.Grade)));

		int powers = Relics.PowerCount(relic.Grade);
		for (int i = 0; i < _inspectPowers.Length; i++)
		{
			bool has = i < powers;
			_inspectPowers[i].SetActive(has);
			if (has)
			{
				Ui.SetText(_inspectPowers[i], "- " + Relics.DescribeOn(relic, i));
				// The power that lends structures is the only one that changes the parish
				// instead of a coefficient, so it is the only one drawn in the colour reserved
				// for what a communion leaves behind.
				Ui.SetTextColor(_inspectPowers[i], Relics.PowerAt(relic, i) == Power.Foundation
					? Palette.Sigil : Palette.Ichor);
			}
		}

		Ui.SetText(_inspectFlavour, Relics.Flavour(relic));
		Ui.SetText(_inspectFoot, _inspectingWorn
			? "Worn. Click the row to take it off."
			: "Carried. Click to wear   -   right-click twice to render for +"
				+ Numbers.Short(_inspectRenders));
		Ui.SetTextColor(_inspectFoot, _inspectingWorn ? Palette.Ichor : Palette.TextFaint);
	}

	/// <summary>Put every row's four columns back where the shared layout wants them.</summary>
	/// <remarks>
	/// Mirrors the rects the rows are BUILT with, and has to keep mirroring them: if a column
	/// moves where it is created and not here, switching tabs would quietly undo the change.
	/// </remarks>
	private void ResetColumns()
	{
		foreach (Row row in _rows)
		{
			if (!row.Title.IsValid)
			{
				continue;
			}
			Ui.SetRect(row.Title, kTextLeft, Typography.Box(6.0f), TextWidth, Typography.Box(24.0f));
			Ui.SetTextWrap(row.Title, false);
			Ui.SetRect(row.Sub, kTextLeft, Typography.Box(32.0f), TextWidth, Typography.Box(23.0f));
			Ui.SetRect(row.Cost, FiguresLeft, Typography.Box(6.0f), FiguresWidth, Typography.Box(24.0f));
			Ui.SetRect(row.Note, FiguresLeft, Typography.Box(32.0f), FiguresWidth, Typography.Box(23.0f));
		}
	}

	/// <summary>Which tab the rows are currently laid out for.</summary>
	private LedgerTab _lastTab = LedgerTab.Rites;

	/// <summary>
	/// Which carried relic is one click away from being rendered, or -1.
	/// </summary>
	/// <remarks>
	/// Rendering used to happen on the first right-click, with nothing said before or after: the
	/// relic was simply gone. A keeper could not tell whether they had rendered it, dropped it,
	/// or hit a bug - and if it was a good one, they could not get it back. The gesture stays a
	/// right-click, but it now takes two, and the row says so in between.
	/// </remarks>
	/// <summary>
	/// The SEED of the relic a right-click has armed for rendering, or zero.
	/// </summary>
	/// <remarks>
	/// The seed and not the row. A satchel index is only meaningful for as long as the satchel
	/// does not move, and between arming and confirming it can: the keeper wears something, or
	/// hands one over, or presses wear-the-best, or a visitation takes one - four of the eight
	/// places that remove an entry take it from the middle, and everything below it shifts up.
	/// The armed row then pointed at a DIFFERENT relic, and the second click destroyed a thing
	/// the keeper had not chosen. That is precisely the outcome the two clicks exist to prevent,
	/// which made the confirmation itself a way to lose something by accident.
	/// <para>
	/// Two relics in one satchel CAN share a seed - every keeper's counter starts at one, so a
	/// relic handed over by another player can collide with one of yours. All that does is light
	/// up both rows: what is rendered is the relic on the row that was clicked, because the
	/// second click reads the satchel at that row rather than the armed value. A duplicated
	/// highlight is a fair price for a confirmation that cannot destroy the wrong thing.
	/// </para>
	/// </remarks>
	private int _renderArmed;

	/// <summary>Which rite is one click away from being consecrated, or -1. Same two-step as
	/// rendering, and deliberately the same gesture: both are choices that cannot be undone, and
	/// a keeper who has learned the arming on one has learned it on the other.</summary>
	private int _consecrateArmed = -1;

	/// <summary>When an armed consecration forgets. Same clock and the same reason as the
	/// rendering deadline below.</summary>
	private float _armedUntil;

	/// <summary>When the armed row forgets, on the UNSCALED clock. A deadline rather than a
	/// countdown because the only delta the SDK exposes is the scaled one, which stops while the
	/// game is paused - and an arming that never expires while paused is an arming a keeper can
	/// come back to an hour later and fire by accident, which is the whole thing being fixed.</summary>
	private float _renderArmedUntil;

	private void HandleScroll()
	{
		// How many rows actually fit is a function of the window height, so it is measured from
		// the resolved viewport rather than assumed. A short window shows fewer rows instead of
		// drawing them under the edge of the screen.
		Vector4 rect = Ui.GetRect(_viewport);
		if (rect.W > 0.0f)
		{
			_visibleRows = Math.Clamp((int)MathF.Floor(rect.W / (kRowHeight + kRowGap)), 1, kRowPool);
		}

		if (Ui.IsHovered(_panel))
		{
			float wheel = Input.ScrollDelta.Y;
			if (MathF.Abs(wheel) > 0.01f)
			{
				_scroll -= (int)MathF.Round(wheel);
			}
		}
		// The transcript grows at the END A READER IS LOOKING AT - index 0 is the newest line - so
		// left alone it slides whatever is being read down a row every time the parish speaks.
		// Anchoring moves the view with the lines rather than holding an offset that no longer
		// points at them. Only the transcript needs it: every other tab's list either does not
		// move or grows at the bottom.
		if (_tab == LedgerTab.Voices)
		{
			_scroll = Transcript.Anchored(_scroll, _seenSaid);
		}
		_seenSaid = Transcript.Added;

		int maxScroll = Math.Max(0, _itemCount - _visibleRows);
		_scroll = Math.Clamp(_scroll, 0, maxScroll);
	}

	private void Fill()
	{
		for (int i = 0; i < kRowPool; i++)
		{
			_rows[i].Box.SetActive(i < _visibleRows);
			// Off by default and switched on only by the relic tab. A pooled decoration that one
			// tab turns on and the others forget to turn off follows the reader around - the
			// same shape as the tithe button that went missing from the congregation.
			_rows[i].Icon.SetActive(false);
		}

		// Every tab shares eleven rows, and the voices tab lays them out differently - one wide
		// wrapped sentence instead of four columns. So the columns are put BACK whenever the tab
		// changes, rather than each tab being trusted to leave them as it found them. This is the
		// same shape of fault as the icon above: a pooled thing one tab alters and the others
		// never restore follows the reader around, and the reader blames the tab they are on.
		if (_tab != _lastTab)
		{
			ResetColumns();
			_lastTab = _tab;
			// Leaving the tab is as clear a "no" as any.
			_renderArmed = 0;
			_consecrateArmed = -1;
		}

		// An armed row forgets on its own. Scrolling disarms too, because the row indices under
		// the pointer have moved and an armed index would point at a different relic.
		if (_renderArmed != 0 && (Time.UnscaledTime >= _renderArmedUntil || _tab != LedgerTab.Relics))
		{
			_renderArmed = 0;
		}
		if (_consecrateArmed >= 0 && (Time.UnscaledTime >= _armedUntil || _tab != LedgerTab.Rites))
		{
			_consecrateArmed = -1;
		}

		// Cleared before the rows are dressed and set by whichever one is under the pointer, so
		// the panel follows the pointer without any row needing to know the panel exists.
		_inspecting = default;

		switch (_tab)
		{
			case LedgerTab.Rites:
				FillRites();
				break;
			case LedgerTab.Offerings:
				FillOfferings();
				break;
			case LedgerTab.Communion:
				FillCommunion();
				break;
			case LedgerTab.Relics:
				FillRelics();
				break;
			case LedgerTab.Voices:
				FillVoices();
				break;
			default:
				FillMarks();
				break;
		}

		DrawInspector();
	}

	// ── Rites ────────────────────────────────────────────────────────────────────────

	/// <summary>A rite the keeper has never been near is shown as a rumour rather than hidden:
	/// an idle game with an invisible next tier gives no reason to keep going.</summary>
	private static bool Revealed(int rite)
		=> Vigil.Owned[rite] > 0 || Vigil.LifetimeIchor >= Content.Rites[rite].BaseCost * 0.35;

	private void FillRites()
	{
		_itemCount = Content.RiteCount;
		int slot = 0;
		for (int rite = _scroll; rite < Content.RiteCount && slot < _visibleRows; rite++, slot++)
		{
			Row row = _rows[slot];
			RiteDef def = Content.Rites[rite];
			int owned = Vigil.Owned[rite];

			if (!Revealed(rite))
			{
				Ui.SetText(row.Title, "???");
				Ui.SetText(row.Sub, "Something further down. You are not deep enough to name it.");
				Ui.SetText(row.Cost, "");
				Ui.SetText(row.Note, "");
				Ui.SetTextColor(row.Title, Palette.TextFaint);
				row.Box.SetEnabled(false);
				row.Box.SetColour(Palette.PanelDeep);
				continue;
			}

			int count = BuyAmount > 0 ? BuyAmount : Math.Max(1, Vigil.Affordable(rite));
			double cost = Vigil.CostOfMany(rite, count);
			bool affordable = cost <= Vigil.Ichor;

			Ui.SetText(row.Title, def.Name + (owned > 0 ? "   x" + owned : ""));
			Ui.SetTextColor(row.Title, owned > 0 ? def.Colour : Palette.TextBright);
			// AftermathScale included, because the HUD's total includes it and two readouts of the
			// same thing must not disagree. Without it, a keeper who had just been visited saw
			// every rite claiming full production while the bar above said half - the ledger
			// being the one that was wrong, and by exactly two.
			// Once a rite is consecrated, every OTHER row has to say what that cost it, or a
			// keeper sees their whole parish quietly producing a fifth less with no explanation.
			if (Vigil.Consecrated >= 0 && Vigil.Consecrated != rite && owned > 0)
			{
				Ui.SetTextColor(row.Title, Palette.TextDim);
			}
			Ui.SetText(row.Sub, owned > 0
				? Numbers.Rate(owned * def.BaseRate * Vigil.RiteMultiplier(rite) * Vigil.AftermathScale) +
				  "   each " + Numbers.Mult(Vigil.RiteMultiplier(rite)) + "   next double at " +
				  ((owned / Content.MilestoneStep + 1) * Content.MilestoneStep)
				: def.Blurb);
			Ui.SetText(row.Cost, Numbers.Short(cost));
			Ui.SetTextColor(row.Cost, affordable ? Palette.Ichor : Palette.TextFaint);
			Ui.SetText(row.Note, "buy " + count + (Vigil.Overseers[rite] ? "   overseen" : ""));

			// The one irreversible choice in a vigil, offered on the row of the thing being
			// chosen rather than behind a menu of its own - it is a statement ABOUT a rite, so
			// it belongs where the keeper is already looking at that rite. Right-click arms it
			// and says what it will cost, exactly as rendering a relic does.
			if (Vigil.Consecrated == rite)
			{
				Ui.SetText(row.Note, "CONSECRATED   x" + Numbers.Mult(Vigil.ConsecratedGain).Substring(1));
				Ui.SetTextColor(row.Note, Palette.Sigil);
			}
			else if (Vigil.Consecrated >= 0)
			{
				Ui.SetTextColor(row.Note, Palette.TextFaint);
			}
			else if (owned > 0)
			{
				bool armed = _consecrateArmed == rite;
				Ui.SetText(row.Note, armed
					? "RIGHT-CLICK AGAIN TO CONSECRATE"
					: "buy " + count + "   right-click to consecrate");
				Ui.SetTextColor(row.Note, armed ? Palette.Sigil : Palette.TextFaint);

				if (Ui.IsHovered(row.Box.Root) && Input.IsMousePressed(MouseButton.Right))
				{
					if (armed)
					{
						Vigil.Consecrate(rite);
						_consecrateArmed = -1;
					}
					else
					{
						_consecrateArmed = rite;
						_armedUntil = Time.UnscaledTime + 3.0f;
					}
				}
			}

			// The working bar: owned rites show their cadence, unowned ones show nothing,
			// because an empty bar on something you do not have is just noise.
			float rowWidth = kWidth - kPad * 2.0f;
			float fill = owned > 0 ? (float)Vigil.CycleProgress[rite] : 0.0f;
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, rowWidth * fill, 3.0f);
			Ui.SetImageColor(row.Progress, Palette.Fade(def.Colour, owned > 0 ? 0.75f : 0.0f));

			row.Box.SetEnabled(affordable);
			row.Box.Style(affordable, Palette.RowHot, Palette.Row, Palette.PanelDeep);

			if (row.Box.Activated && affordable)
			{
				Vigil.BuyRite(rite, count);
			}
		}
		BlankFrom(slot);
	}

	// ── Offerings ────────────────────────────────────────────────────────────────────

	private void FillOfferings()
	{
		// Rebuilt each frame from the live availability test rather than cached: an offering
		// becomes available the instant a milestone lands, and a cached list would show it a
		// tab-switch later.
		Span<int> available = stackalloc int[Content.Offerings.Length];
		int count = 0;
		for (int i = 0; i < Content.Offerings.Length; i++)
		{
			if (Vigil.OfferingAvailable(i))
			{
				available[count++] = i;
			}
		}
		_itemCount = count;

		int slot = 0;
		for (int i = _scroll; i < count && slot < _visibleRows; i++, slot++)
		{
			int index = available[i];
			OfferingDef def = Content.Offerings[index];
			Row row = _rows[slot];

			bool affordable = def.Cost <= Vigil.Ichor;
			Ui.SetText(row.Title, def.Name);
			Ui.SetTextColor(row.Title, Palette.TextBright);
			Ui.SetText(row.Sub, def.Blurb);
			Ui.SetText(row.Cost, Numbers.Short(def.Cost));
			Ui.SetTextColor(row.Cost, affordable ? Palette.Ichor : Palette.TextFaint);
			// Both halves of what it does, or the figures column lies.
			//
			// It showed the multiplier alone, which was true while every offering was only a
			// multiplier and became a misreport the moment one of them cost something: the
			// offering that quiets a rite showed "x0.85" and never mentioned the dread it takes
			// away, so the only reason to buy it was invisible and the row read as strictly bad.
			// The one that leans into the dark showed "x1.40" and hid its price, which is worse -
			// a row that looks like a pure gain and is not. The blurb says both in words; the
			// figures have to agree with the words.
			string effect = Numbers.Mult(def.Multiplier) + (def.Target == OfferingDef.TargetGlobal
				? " everything"
				: def.Target == OfferingDef.TargetHand ? " by hand" : "");
			if (def.DreadScale != 1.0)
			{
				effect += "   dread " + Numbers.Mult(def.DreadScale);
			}
			Ui.SetText(row.Note, effect);
			// A bargain that raises the dread is the only row in the ledger that carries a
			// warning, so it is the only one drawn in the colour the game uses for the dark.
			Ui.SetTextColor(row.Note, def.DreadScale > 1.0 ? Palette.DreadText
				: def.DreadScale < 1.0 ? Palette.Ichor : Palette.TextFaint);

			row.Box.SetEnabled(affordable);
			row.Box.Style(affordable, Palette.RowHot, Palette.Row, Palette.PanelDeep);

			if (row.Box.Activated && affordable)
			{
				Vigil.TakeOffering(index);
			}
		}

		if (count == 0 && _visibleRows > 0)
		{
			Row row = _rows[0];
			Ui.SetText(row.Title, "Nothing is offered.");
			Ui.SetText(row.Sub, "Keep more of a rite, or gather more, and the parish will ask.");
			Ui.SetText(row.Cost, "");
			Ui.SetText(row.Note, "");
			Ui.SetTextColor(row.Title, Palette.TextFaint);
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
			slot = 1;
		}
		BlankFrom(slot);
	}

	// ── Communion ────────────────────────────────────────────────────────────────────

	/// <summary>The communion tab is one list of three kinds of row - the communion itself,
	/// then the boons, then the overseers - so scrolling and hit-testing stay the ledger's one
	/// model rather than three that have to agree.</summary>
	private void FillCommunion()
	{
		_itemCount = 1 + Content.Boons.Length + Content.RiteCount;
		int slot = 0;

		if (_scroll == 0 && slot < _visibleRows)
		{
			int payout = Vigil.SigilsOnOffer;
			bool ready = payout > 0;
			Row row = _rows[slot];
			// What the payout is worth AGAINST WHAT IS ALREADY HELD, because that is the whole
			// decision and the raw number hides it. Simulated over twelve hours, a keeper who
			// communes the moment it pays anything finishes twenty-four MILLION times behind one
			// who waits for the offer to be worth half again what they have - the harshest
			// consequence in the game, attached to a button that said only "+4 sigils".
			double already = Math.Max(1, Vigil.SigilsEarned);
			double share = payout / already;
			bool worthIt = share >= 0.25;

			Ui.SetText(row.Title, "COMMUNE   +" + payout + " sigils");
			Ui.SetTextColor(row.Title, !ready ? Palette.TextFaint : worthIt ? Palette.Sigil : Palette.TextDim);
			Ui.SetText(row.Sub, ready
				? worthIt
					? "Give the parish back. You keep the sigils, the boons, the overseers and the marks."
					: "Only " + Numbers.Percent(share) + " more than you have already taken. The parish is worth more standing."
				: "Gather " + Numbers.Short(NextSigilAt()) + " this run for the first sigil.");
			// Held and earned, both, because the difference between them is the one rule of this
			// tab a player has to trust: spending sigils never costs you the bonus they pay.
			Ui.SetText(row.Cost, Vigil.Sigils + " held  /  " + Vigil.SigilsEarned + " taken");
			Ui.SetTextColor(row.Cost, Palette.Sigil);
			// The MULTIPLIER, before and after, because that is the only thing a sigil does and
			// this row is the only place it is quoted. It used to read "+X% on what you hold",
			// where X was the payout as a share of sigils TAKEN - which is neither what is held
			// nor what the bonus does. The bonus is linear in the count and starts at one, so
			// early on the two diverge badly: a keeper with ten taken and five on offer was told
			// "+50%" for a bonus that went from x1.60 to x1.90, which is nineteen. The share is
			// still the right number for the decision and still stated in the line above; this
			// says what the decision buys.
			Ui.SetText(row.Note, ready && Vigil.SigilsEarned > 0
				? Numbers.Mult(Vigil.SigilMultiplier) + "  ->  "
					+ Numbers.Mult(Vigil.SigilMultiplierFor(Vigil.SigilsEarned + payout))
				: Numbers.Mult(Vigil.SigilMultiplier) + " from every sigil taken");
			Ui.SetTextColor(row.Note, ready && !worthIt ? Palette.TextFaint : Palette.Sigil);
			row.Box.SetEnabled(ready);
			row.Box.Style(ready, Palette.Mix(Palette.RowHot, Palette.Sigil, 0.28f), Palette.Row, Palette.PanelDeep);
			if (row.Box.Activated && ready)
			{
				Vigil.Commune();
			}
			slot++;
		}

		// ── Boons ────────────────────────────────────────────────────────────────────
		for (int i = Math.Max(0, _scroll - 1); i < Content.Boons.Length && slot < _visibleRows; i++, slot++)
		{
			Row row = _rows[slot];
			BoonDef def = Content.Boons[i];
			int level = Vigil.Boons[i];
			int cost = Vigil.BoonCost(i);
			bool maxed = level >= def.MaxLevel;
			bool affordable = !maxed && Vigil.Sigils >= cost;

			// A short ladder shows how much of it is done; the long one shows only how far the
			// keeper has come. Two hundred is a safety limit rather than a target - nobody
			// reaches it and nobody is meant to - so printing "12/200" would answer a question
			// the keeper is not asking and answer it discouragingly, six percent of a goal that
			// is not a goal.
			bool longLadder = def.MaxLevel > 10;
			Ui.SetText(row.Title, def.Name + "   " + level + (longLadder ? "" : "/" + def.MaxLevel));
			Ui.SetTextColor(row.Title, level > 0 ? Palette.Sigil : Palette.TextBright);
			Ui.SetText(row.Sub, def.Blurb);
			Ui.SetText(row.Cost, maxed ? "KEPT" : cost + " sigils");
			Ui.SetTextColor(row.Cost, maxed ? Palette.Ichor : affordable ? Palette.Sigil : Palette.TextFaint);
			Ui.SetText(row.Note, "");

			// The level pips: the same thin fill a rite uses for its working, standing in for
			// how far along a boon is. One widget, two meanings, no third layout to keep.
			float boonWidth = kWidth - kPad * 2.0f;
			// The long ladder's bar fills toward the NEXT ten rather than toward two hundred, so
			// it keeps meaning something instead of sitting near empty forever. A bar that never
			// visibly moves is worse than no bar.
			float along = longLadder
				? level % 10 / 10.0f
				: (float)level / def.MaxLevel;
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, boonWidth * along, 3.0f);
			Ui.SetImageColor(row.Progress, Palette.Fade(Palette.Sigil, level > 0 ? 0.75f : 0.0f));

			row.Box.SetEnabled(affordable);
			row.Box.Style(affordable, Palette.RowHot, maxed ? Palette.PanelDeep : Palette.Row, Palette.PanelDeep);

			if (row.Box.Activated && affordable)
			{
				Vigil.BuyBoon(i);
			}
		}

		// ── Overseers ────────────────────────────────────────────────────────────────
		for (int rite = Math.Max(0, _scroll - 1 - Content.Boons.Length); rite < Content.RiteCount && slot < _visibleRows; rite++, slot++)
		{
			Row row = _rows[slot];
			int cost = Vigil.OverseerCost(rite);
			bool hired = Vigil.Overseers[rite];
			bool affordable = !hired && Vigil.Sigils >= cost;

			Ui.SetText(row.Title, "Overseer: " + Content.Rites[rite].Name);
			Ui.SetTextColor(row.Title, hired ? Content.Rites[rite].Colour : Palette.TextBright);
			Ui.SetText(row.Sub, hired
				? "It buys for you, out of surplus, and never touches the ward money."
				: "Hire someone to keep this rite topped up while you are elsewhere.");
			Ui.SetText(row.Cost, hired ? "HIRED" : cost + " sigils");
			Ui.SetTextColor(row.Cost, hired ? Palette.Ichor : affordable ? Palette.Sigil : Palette.TextFaint);
			Ui.SetText(row.Note, "");
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f);
			row.Box.SetEnabled(affordable);
			row.Box.Style(affordable, Palette.RowHot, hired ? Palette.PanelDeep : Palette.Row, Palette.PanelDeep);

			if (row.Box.Activated && affordable)
			{
				Vigil.HireOverseer(rite);
			}
		}
		BlankFrom(slot);
	}

	/// <summary>Run ichor needed for the next whole sigil. Asked of the simulation rather than
	/// re-derived here: this used to be a hand-inverted copy of the payout curve, which is
	/// exactly the kind of duplicate that goes quietly wrong the day the curve is retuned.</summary>
	private static double NextSigilAt() => Vigil.RunIchorForSigils(Vigil.SigilsOnOffer + 1);

	// ── Relics ───────────────────────────────────────────────────────────────────────

	/// <summary>
	/// What the keeper is wearing, then what they are carrying.
	/// </summary>
	/// <remarks>
	/// The worn slots come first and always, empty or not, so the three things a keeper can
	/// have on them are a fixed shape at the top of the page rather than something that moves
	/// as the satchel fills. Clicking a worn slot takes it off; clicking a carried relic puts
	/// it on. One click, one meaning, in a list a player is going to be clicking quickly.
	/// </remarks>
	private void FillRelics()
	{
		_itemCount = Relics.Slots + Vigil.Satchel.Count;
		int slot = 0;

		for (int worn = _scroll; worn < Relics.Slots && slot < _visibleRows; worn++, slot++)
		{
			Row row = _rows[slot];
			Relic relic = Vigil.Worn[worn];
			if (!relic.Exists)
			{
				Ui.SetText(row.Title, "An empty hand");
				Ui.SetTextColor(row.Title, Palette.TextFaint);
				Ui.SetText(row.Sub, "Anything you are carrying can go here.");
				Ui.SetText(row.Cost, "");
				Ui.SetText(row.Note, "");
				row.Box.SetEnabled(false);
				row.Box.SetColour(Palette.PanelDeep);
				continue;
			}

			DressRelic(row, relic, worn: true);
			row.Box.SetEnabled(true);
			row.Box.Style(true, Palette.RowHot, Palette.Row, Palette.PanelDeep);
			if (row.Box.Activated)
			{
				Vigil.Remove(worn);
			}
		}

		for (int i = Math.Max(0, _scroll - Relics.Slots); i < Vigil.Satchel.Count && slot < _visibleRows; i++, slot++)
		{
			Row row = _rows[slot];
			Relic relic = Vigil.Satchel[i];
			DressRelic(row, relic, worn: false);
			// The first carried relic is the one the congregation's RELIC button hands over. It
			// always was - there is no room for a target picker in a 320px row - but nothing
			// said so, which made trading a surprise rather than a choice. Saying it turns the
			// rule into something a keeper can steer with the two clicks they already have.
			// What rendering it pays, on the row, because a price nobody can see is one nobody
			// weighs - and the first carried relic is also the one the congregation hands over.
			if (Ui.IsHovered(row.Box.Root))
			{
				_inspectRenders = Vigil.RenderValue(i);
			}
			Ui.SetText(row.Note, (i == 0 ? "next to give   " : "") +
				"render +" + Numbers.Short(Vigil.RenderValue(i)));
			Ui.SetTextColor(row.Note, i == 0 ? Palette.Sigil : Palette.IchorDim);
			row.Box.SetEnabled(true);
			row.Box.Style(true, Palette.RowHot, Palette.Row, Palette.PanelDeep);

			// Right-click renders it down for ichor rather than binning it. Asking somebody to
			// throw away a thing they went and found is a poor trade even when the thing is
			// junk; melting it is the same tidying gesture with something to show for it, and
			// it is still how a keeper chooses what the next offer will be.
			//
			// TWICE, though. The first click arms the row and says what it is about to do and
			// what it pays; the second does it. One click destroying a thing the keeper spent
			// an hour finding, with no warning and no undo, is the kind of interaction people
			// remember a game for and not fondly.
			if (_renderArmed == relic.Seed)
			{
				Ui.SetText(row.Note, "RIGHT-CLICK AGAIN   +" + Numbers.Short(Vigil.RenderValue(i)));
				Ui.SetTextColor(row.Note, Palette.DreadText);
			}

			if (Ui.IsHovered(row.Box.Root) && Input.IsMousePressed(MouseButton.Right))
			{
				if (_renderArmed == relic.Seed)
				{
					// Vigil.Render says what it did, so the confirmation lands in the feed and
					// stays in the transcript. No event back to the game for it: the parish's
					// voice is the channel every other consequence already uses, and a line a
					// keeper can scroll back to beats a pop they might blink through.
					Vigil.Render(i);
					_renderArmed = 0;
				}
				else
				{
					_renderArmed = relic.Seed;
					// Long enough to read the row, short enough that it cannot be armed now and
					// fired by an unrelated click later.
					_renderArmedUntil = Time.UnscaledTime + 3.0f;
				}
				break;
			}

			if (row.Box.Activated)
			{
				// Into the first empty hand, or the first one if every hand is full. Choosing a
				// slot would be a second click for a decision almost nobody wants to make.
				int into = 0;
				for (int s = 0; s < Relics.Slots; s++)
				{
					if (!Vigil.Worn[s].Exists)
					{
						into = s;
						break;
					}
				}
				Vigil.Wear(i, into);
			}
		}

		if (_itemCount == Relics.Slots && Vigil.Satchel.Count == 0 && slot < _visibleRows)
		{
			Row row = _rows[slot];
			Ui.SetText(row.Title, "You are carrying nothing.");
			Ui.SetText(row.Sub, "Gather by hand. The deeper in the dark you are, the better what you turn up.");
			Ui.SetText(row.Cost, "");
			Ui.SetText(row.Note, "");
			Ui.SetTextColor(row.Title, Palette.TextFaint);
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
			slot++;
		}
		BlankFrom(slot);
	}

	/// <summary>Put one relic on a row: its drawing, its name, what it does, and its grade.</summary>
	private void DressRelic(Row row, Relic relic, bool worn)
	{
		row.Icon.SetActive(true);
		if (Ui.IsHovered(row.Box.Root))
		{
			_inspecting = relic;
			_inspectingWorn = worn;
		}
		// Seed and grade, exactly what the C# generator works from - so the drawing and the
		// name can never disagree about which relic this is.
		// ArtSeed, not the seed: a float4 carries float32, which holds integers exactly only to
		// about sixteen million, and relic seeds run to a billion.
		Ui.SetMaterialParams(row.Icon, new Vector4(Time.UnscaledTime, Relics.ArtSeed(relic),
			(float)(int)relic.Grade, worn ? 1.0f : 0.35f));
		// The ichor's ALPHA carries which powers this relic has, one bit each, so the drawing
		// is built from the same facts as the description beside it. See Relics.PackedPowerMask;
		// only the rgb is ever read as a colour.
		Ui.SetMaterialColors(row.Icon, Palette.Fade(Palette.Ichor, Relics.PackedPowerMask(relic)),
			Palette.Dread);

		Ui.SetText(row.Title, Relics.NameOf(relic));
		// Grade shown by how much ichor the name carries, not by a colour of its own: the
		// palette allows three accents and a rarity ramp is not one of them.
		Ui.SetTextColor(row.Title, Palette.Mix(Palette.TextDim, Palette.Ichor, Hud.GradeWeight(relic.Grade)));

		string powers = "";
		for (int i = 0; i < Relics.PowerCount(relic.Grade); i++)
		{
			powers += (powers.Length > 0 ? "   " : "") +
				Relics.DescribeOn(relic, i);
		}
		Ui.SetText(row.Sub, powers);

		// The grade spelled out AND counted in marks. Playtested, a keeper could not tell what
		// they were holding: one word in a slightly different shade is not a rarity, and shades
		// cannot be compared across two rows that are not next to each other.
		Ui.SetText(row.Cost, Relics.GradeName(relic.Grade).ToUpperInvariant() + Hud.Pips(relic.Grade));
		Ui.SetTextColor(row.Cost, Palette.Mix(Palette.TextFaint, Palette.Ichor, Hud.GradeWeight(relic.Grade)));
		Ui.SetText(row.Note, worn ? "worn" : "carried");
		Ui.SetTextColor(row.Note, Palette.TextFaint);
		// The row's own fill, used here as a rarity bar - the same widget the rites use for a
		// working and the boons for their levels. A length can be compared down a column at a
		// glance, which is the thing a shade of green cannot do.
		float rowWidth = kWidth - kPad * 2.0f;
		Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f,
			rowWidth * ((int)relic.Grade + 1) / (float)((int)Grade.Hollowed + 1), 3.0f);
		Ui.SetImageColor(row.Progress, Palette.Fade(
			Palette.Mix(Palette.IchorDim, Palette.Ichor, Hud.GradeWeight(relic.Grade)), 0.85f));
	}

	// ── Voices ───────────────────────────────────────────────────────────────────────

	/// <summary>
	/// Everything the parish has said, newest first.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The feed holds a line for nine seconds and then it is gone. That is the right way to
	/// speak - a wall of standing text is not a voice - but it made the only part of the game
	/// with anything to say the only part a keeper could not go back to. Look away for a minute
	/// and a thing was said to you that you cannot now read.
	/// </para>
	/// <para>
	/// Laid out differently from every other tab, because a row here is a sentence rather than
	/// four columns of figures: the line takes the whole width and wraps, and the only thing in
	/// the right-hand column is when it was said. The columns the other tabs use are put back
	/// on the way out, since eleven rows are shared between all six tabs.
	/// </para>
	/// </remarks>
	private void FillVoices()
	{
		_itemCount = Transcript.Count;
		int slot = 0;

		for (int i = _scroll; i < Transcript.Count && slot < _visibleRows; i++, slot++)
		{
			Row row = _rows[slot];
			(string line, Omen omen, double at) = Transcript.At(i);

			// The whole row for the sentence, wrapped, and the clock alone on the right.
			Ui.SetRect(row.Title, kTextLeft - 46.0f, Typography.Box(6.0f),
				kWidth - kPad * 2.0f - (kTextLeft - 46.0f) - FiguresWidth - kColumnGap,
				Typography.Box(52.0f));
			Ui.SetTextWrap(row.Title, true);
			Ui.SetText(row.Title, line);
			Ui.SetTextColor(row.Title, ColourOf(omen));

			Ui.SetText(row.Sub, "");
			Ui.SetText(row.Cost, Numbers.Duration(at));
			Ui.SetTextColor(row.Cost, Palette.TextFaint);
			Ui.SetText(row.Note, "");

			row.Icon.SetActive(false);
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f);
			// Nothing here is bought, so nothing here is a button. Left focusable would put
			// eleven dead stops in the d-pad's path through the ledger.
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
		}

		if (Transcript.Count == 0 && _visibleRows > 0)
		{
			Row row = _rows[0];
			Ui.SetText(row.Title, "The parish has said nothing yet.");
			Ui.SetText(row.Sub, "It will. Everything it says is kept here, and kept through a communion.");
			Ui.SetText(row.Cost, "");
			Ui.SetText(row.Note, "");
			Ui.SetTextColor(row.Title, Palette.TextFaint);
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
			slot = 1;
		}
		BlankFrom(slot);
	}

	/// <summary>An omen as a colour. The same three the feed uses, so a line means the same
	/// thing read back as it did when it was said.</summary>
	private static Vector4 ColourOf(Omen omen) => omen switch
	{
		Omen.Good => Palette.Ichor,
		Omen.Dread => Palette.DreadText,
		Omen.Taken => Palette.DreadText,
		_ => Palette.TextDim,
	};

	// ── Marks ────────────────────────────────────────────────────────────────────────

	/// <summary>How a verb reads on the page the keeper keeps it on.</summary>
	private static string AnswerWord(Answer answer) => answer switch
	{
		Answer.Ward => "a ward",
		Answer.Offer => "an offering",
		Answer.Bell => "the bell",
		Answer.Still => "standing still",
		_ => "",
	};

	private void FillMarks()
	{
		// The record, then the marks, then everything that has come for this keeper.
		_itemCount = 1 + Content.Marks.Length + Content.RiteCount;
		int slot = 0;

		if (_scroll == 0 && slot < _visibleRows)
		{
			Row row = _rows[slot];
			Ui.SetText(row.Title, "THE RECORD");
			Ui.SetTextColor(row.Title, Palette.TextDim);
			Ui.SetText(row.Sub, "Kept " + Numbers.Duration(Vigil.PlayedSeconds) + "   " +
				Vigil.Communions + " communions   " + Vigil.TimesTaken + " taken   " +
				Vigil.WardsRaised + " wards");
			Ui.SetText(row.Cost, Numbers.Short(Vigil.LifetimeIchor));
			Ui.SetTextColor(row.Cost, Palette.Ichor);
			Ui.SetText(row.Note, "all told");
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
			slot++;
		}

		for (int i = Math.Max(0, _scroll - 1); i < Content.Marks.Length && slot < _visibleRows; i++, slot++)
		{
			Row row = _rows[slot];
			bool earned = Vigil.MarksEarned[i];
			Ui.SetText(row.Title, Content.Marks[i].Name);
			Ui.SetTextColor(row.Title, earned ? Palette.Ichor : Palette.TextFaint);
			Ui.SetText(row.Sub, Content.Marks[i].Blurb);
			Ui.SetText(row.Cost, earned ? "KEPT" : "");
			Ui.SetTextColor(row.Cost, Palette.IchorDim);
			// Say when a mark needs other people. Two of them cannot be earned alone, and left
			// unlabelled they read as a completionist's dead end rather than as an invitation.
			Ui.SetText(row.Note, !earned && Content.Marks[i].NeedsCongregation ? "needs a congregation" : "");
			Ui.SetTextColor(row.Note, Palette.Sigil);

			// How far along, for the marks that count something. The same bar the relics use for
			// rarity and the rites use for a working, so a length means the same thing wherever
			// it appears - and drawn only while the mark is unearned, because a full bar under a
			// mark already marked KEPT is one more thing to read that says nothing.
			float rowWidth = kWidth - kPad * 2.0f;
			double toward = !earned && Content.Marks[i].Toward != null
				? Math.Clamp(Content.Marks[i].Toward!(), 0.0, 1.0)
				: 0.0;
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, rowWidth * (float)toward, 3.0f);
			Ui.SetImageColor(row.Progress, Palette.Fade(Palette.IchorDim, 0.85f));
			// Never over the congregation note: "needs a congregation" is the more useful of
			// the two, since it explains why the bar is not moving.
			if (!earned && Content.Marks[i].Toward != null && !Content.Marks[i].NeedsCongregation)
			{
				Ui.SetText(row.Note, Numbers.Percent(toward));
				Ui.SetTextColor(row.Note, Palette.TextFaint);
			}
			row.Box.SetEnabled(false);
			row.Box.SetColour(earned ? Palette.Row : Palette.PanelDeep);
		}

		// ── What has come for you ────────────────────────────────────────────────────
		// The keeper's own notes, and the only place the answers are ever written down. A
		// visitor is a rumour until you meet it, a name once you have, and an ANSWER only once
		// you have turned it away yourself - so the page fills in as the encounters teach it,
		// and never hands over a verb the keeper has not earned. That ordering is the whole
		// reason this is a bestiary rather than a hint list.
		for (int i = Math.Max(0, _scroll - 1 - Content.Marks.Length); i < Content.RiteCount && slot < _visibleRows; i++, slot++)
		{
			Row row = _rows[slot];
			RiteDef def = Content.Rites[i];
			bool met = Vigil.VisitorsMet[i];
			bool bested = Vigil.VisitorsBested[i];

			Ui.SetText(row.Title, met ? def.VisitorName : "Something else");
			Ui.SetTextColor(row.Title, bested ? def.Colour : met ? Palette.TextBright : Palette.TextFaint);
			Ui.SetText(row.Sub, bested
				? def.Approach
				: met
					? "You have seen it. You have not yet learned what it wants."
					: "The " + def.Name + " has not called anything to you yet.");
			Ui.SetText(row.Cost, bested ? "ANSWERED" : "");
			Ui.SetTextColor(row.Cost, Palette.Ichor);
			// The answer, and only once it has been proved - being taken by something teaches
			// you nothing about it.
			Ui.SetText(row.Note, bested ? "turned by " + AnswerWord(def.Answer) : "");
			Ui.SetTextColor(row.Note, Palette.TextDim);
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f);
			row.Box.SetEnabled(false);
			row.Box.SetColour(bested ? Palette.Row : Palette.PanelDeep);
		}
		BlankFrom(slot);
	}

	/// <summary>
	/// Switch off every pooled row a tab did not use.
	/// </summary>
	/// <remarks>
	/// Two jobs, and the second one is the load-bearing one: a short list must not leave the
	/// previous tab's text sitting under it, and a row left ACTIVE is a row that can still be
	/// clicked. Together with each fill method reading its own <c>row.Box.Activated</c> inside
	/// the same iteration that populated it - with the live index in scope, never from a
	/// remembered one - that is the whole of what stops a click landing on a row that has since
	/// come to mean something else. There is no separate bookkeeping guarding this, so do not
	/// hoist an Activated check out of a fill loop and do not skip this call on an early out.
	/// </remarks>
	private void BlankFrom(int slot)
	{
		for (int i = slot; i < kRowPool; i++)
		{
			_rows[i].Box.SetActive(false);
		}
	}
}
