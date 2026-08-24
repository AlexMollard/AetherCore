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
	private readonly Row[] _rows = new Row[kRowPool];

	/// <summary>Tab labels, kept here because the tab now has a mark appended when it has
	/// something waiting, and the scene's authored label is no longer the whole story.</summary>
	private static readonly string[] s_tabNames = { "RITES", "OFFERINGS", "COMMUNION", "MARKS", "RELICS" };

	private static readonly int[] s_amounts = { 1, 10, 100, -1 };
	private static readonly string[] s_amountLabels = { "x1", "x10", "x100", "MAX" };

	private LedgerTab _tab = LedgerTab.Rites;
	private int _amountIndex;
	private int _scroll;
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
			default:
				FillMarks();
				break;
		}
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
			Ui.SetText(row.Sub, owned > 0
				? Numbers.Rate(owned * def.BaseRate * Vigil.RiteMultiplier(rite) * Vigil.AftermathScale) +
				  "   each " + Numbers.Mult(Vigil.RiteMultiplier(rite)) + "   next double at " +
				  ((owned / Content.MilestoneStep + 1) * Content.MilestoneStep)
				: def.Blurb);
			Ui.SetText(row.Cost, Numbers.Short(cost));
			Ui.SetTextColor(row.Cost, affordable ? Palette.Ichor : Palette.TextFaint);
			Ui.SetText(row.Note, "buy " + count + (Vigil.Overseers[rite] ? "   overseen" : ""));

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
			Ui.SetText(row.Note, Numbers.Mult(def.Multiplier) + (def.Target == OfferingDef.TargetGlobal
				? " everything"
				: def.Target == OfferingDef.TargetHand ? " by hand" : ""));

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
			Ui.SetText(row.Note, ready && Vigil.SigilsEarned > 0
				? "+" + Numbers.Percent(share) + " on what you hold"
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

			Ui.SetText(row.Title, def.Name + "   " + level + "/" + def.MaxLevel);
			Ui.SetTextColor(row.Title, level > 0 ? Palette.Sigil : Palette.TextBright);
			Ui.SetText(row.Sub, def.Blurb);
			Ui.SetText(row.Cost, maxed ? "KEPT" : cost + " sigils");
			Ui.SetTextColor(row.Cost, maxed ? Palette.Ichor : affordable ? Palette.Sigil : Palette.TextFaint);
			Ui.SetText(row.Note, "");

			// The level pips: the same thin fill a rite uses for its working, standing in for
			// how far along a boon is. One widget, two meanings, no third layout to keep.
			float boonWidth = kWidth - kPad * 2.0f;
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f,
				boonWidth * ((float)level / def.MaxLevel), 3.0f);
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
			Ui.SetText(row.Note, (i == 0 ? "next to give   " : "") +
				"render +" + Numbers.Short(Vigil.RenderValue(i)));
			Ui.SetTextColor(row.Note, i == 0 ? Palette.Sigil : Palette.IchorDim);
			row.Box.SetEnabled(true);
			row.Box.Style(true, Palette.RowHot, Palette.Row, Palette.PanelDeep);

			// Right-click renders it down for ichor rather than binning it. Asking somebody to
			// throw away a thing they went and found is a poor trade even when the thing is
			// junk; melting it is the same tidying gesture with something to show for it, and
			// it is still how a keeper chooses what the next offer will be.
			if (Ui.IsHovered(row.Box.Root) && Input.IsMousePressed(MouseButton.Right))
			{
				Vigil.Render(i);
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
		// Seed and grade, exactly what the C# generator works from - so the drawing and the
		// name can never disagree about which relic this is.
		// ArtSeed, not the seed: a float4 carries float32, which holds integers exactly only to
		// about sixteen million, and relic seeds run to a billion.
		Ui.SetMaterialParams(row.Icon, new Vector4(Time.UnscaledTime, Relics.ArtSeed(relic),
			(float)(int)relic.Grade, worn ? 1.0f : 0.35f));
		Ui.SetMaterialColors(row.Icon, Palette.Ichor, Palette.Dread);

		Ui.SetText(row.Title, Relics.NameOf(relic));
		// Grade shown by how much ichor the name carries, not by a colour of its own: the
		// palette allows three accents and a rarity ramp is not one of them.
		Ui.SetTextColor(row.Title, Palette.Mix(Palette.TextDim, Palette.Ichor, Hud.GradeWeight(relic.Grade)));

		string powers = "";
		for (int i = 0; i < Relics.PowerCount(relic.Grade); i++)
		{
			powers += (powers.Length > 0 ? "   " : "") +
				Relics.Describe(Relics.PowerAt(relic, i), Relics.MagnitudeAt(relic, i));
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
			Ui.SetRect(row.Progress, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f);
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
