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
	private const float kRowHeight = 66.0f;
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
	}

	private Entity _panel;
	private Entity _viewport;
	private readonly Button[] _tabs = new Button[4];
	private readonly Button[] _amounts = new Button[4];
	private readonly Row[] _rows = new Row[kRowPool];

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
			row.Title = UiKit.Text(row.Box.Root, "", 14.0f, 8.0f, 370.0f, 24.0f, 17.0f, Palette.TextBright);
			row.Sub = UiKit.Text(row.Box.Root, "", 14.0f, 34.0f, 420.0f, 24.0f, 15.0f, Palette.TextFaint,
				UiHAlign.Left, Palette.Body);
			row.Cost = UiKit.Text(row.Box.Root, "", rowWidth - 190.0f, 8.0f, 176.0f, 24.0f, 17.0f,
				Palette.Ichor, UiHAlign.Right);
			row.Note = UiKit.Text(row.Box.Root, "", rowWidth - 190.0f, 34.0f, 176.0f, 24.0f, 15.0f,
				Palette.TextFaint, UiHAlign.Right);
			row.Progress = UiKit.Image(row.Box.Root, 0.0f, kRowHeight - 3.0f, 0.0f, 3.0f, Palette.IchorDim);
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
			_tabs[i].SetColour(active ? Palette.RowHot : Palette.Row);
			_tabs[i].SetLabelColour(active ? Palette.Ichor : Palette.TextDim);
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
			Ui.SetText(row.Sub, owned > 0
				? Numbers.Rate(owned * def.BaseRate * Vigil.RiteMultiplier(rite)) + "   each " +
				  Numbers.Mult(Vigil.RiteMultiplier(rite)) + "   next double at " +
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
			Ui.SetText(row.Title, "COMMUNE   +" + payout + " sigils");
			Ui.SetTextColor(row.Title, ready ? Palette.Sigil : Palette.TextFaint);
			Ui.SetText(row.Sub, ready
				? "Give the parish back. You keep the sigils, the boons, the overseers and the marks."
				: "Gather " + Numbers.Short(NextSigilAt()) + " this run for the first sigil.");
			// Held and earned, both, because the difference between them is the one rule of this
			// tab a player has to trust: spending sigils never costs you the bonus they pay.
			Ui.SetText(row.Cost, Vigil.Sigils + " held  /  " + Vigil.SigilsEarned + " taken");
			Ui.SetTextColor(row.Cost, Palette.Sigil);
			Ui.SetText(row.Note, Numbers.Mult(Vigil.SigilMultiplier) + " from every sigil taken");
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
			Ui.SetText(row.Note, "");
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
