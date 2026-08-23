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
	private const float kPad = 16.0f;
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
	}

	private Entity _panel;
	private Entity _viewport;
	private readonly Button[] _tabs = new Button[4];
	private readonly Button[] _amounts = new Button[4];
	private readonly Row[] _rows = new Row[kRowPool];
	/// <summary>What each visible row currently stands for. -1 means the slot is empty this
	/// frame, which is also what stops a stale click from buying the wrong thing.</summary>
	private readonly int[] _payload = new int[kRowPool];

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

	public void Build(Entity canvas)
	{
		_panel = UiKit.Stretch(canvas, new Vector2(1.0f, 0.0f), new Vector2(1.0f, 1.0f),
			new Vector2(-kWidth, 0.0f), new Vector2(0.0f, 0.0f), Palette.Panel);

		UiKit.Text(_panel, "THE LEDGER", kPad + 4.0f, 18.0f, 300.0f, 30.0f, 21.0f, Palette.BoneDim,
			UiHAlign.Left, Palette.Display);

		string[] tabNames = { "RITES", "OFFERINGS", "COMMUNION", "MARKS" };
		float tabWidth = (kWidth - kPad * 2.0f - 3.0f * 6.0f) / 4.0f;
		for (int i = 0; i < _tabs.Length; i++)
		{
			_tabs[i] = UiKit.MakeButton(_panel, tabNames[i], kPad + i * (tabWidth + 6.0f), 56.0f,
				tabWidth, 36.0f, 15.0f, Palette.Display);
		}

		for (int i = 0; i < _amounts.Length; i++)
		{
			_amounts[i] = UiKit.MakeButton(_panel, s_amountLabels[i], kPad + i * 78.0f, 102.0f,
				72.0f, 32.0f, 16.0f, Palette.Display);
		}

		// The viewport clips its whole subtree, so a row scrolled past the top is cut off at
		// the pixel rather than drawn over the tabs.
		_viewport = UiKit.Stretch(_panel, new Vector2(0.0f, 0.0f), new Vector2(1.0f, 1.0f),
			new Vector2(kPad, kListTop), new Vector2(-kPad, -kPad), Palette.Transparent);
		_viewport.Component("UI Mask").Add();

		float rowWidth = kWidth - kPad * 2.0f;
		for (int i = 0; i < kRowPool; i++)
		{
			Row row = default;
			row.Box = UiKit.MakeButton(_viewport, "", 0.0f, i * (kRowHeight + kRowGap), rowWidth, kRowHeight);
			// The button's own centred label is unused here: a ledger row has four columns, so
			// they are placed individually and the pooled label is emptied.
			Ui.SetText(row.Box.Label, "");
			row.Title = UiKit.Text(row.Box.Root, "", 14.0f, 8.0f, 370.0f, 24.0f, 17.0f, Palette.Bone);
			row.Sub = UiKit.Text(row.Box.Root, "", 14.0f, 34.0f, 420.0f, 24.0f, 15.0f, Palette.BoneFaint,
				UiHAlign.Left, Palette.Body);
			row.Cost = UiKit.Text(row.Box.Root, "", rowWidth - 190.0f, 8.0f, 176.0f, 24.0f, 17.0f,
				Palette.Ichor, UiHAlign.Right);
			row.Note = UiKit.Text(row.Box.Root, "", rowWidth - 190.0f, 34.0f, 176.0f, 24.0f, 15.0f,
				Palette.BoneFaint, UiHAlign.Right);
			_rows[i] = row;
			_payload[i] = -1;
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
			_tabs[i].SetLabelColour(active ? Palette.Ichor : Palette.BoneDim);
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
			_amounts[i].SetLabelColour(active ? Palette.Ichor : Palette.BoneDim);
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
			_payload[i] = -1;
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
			_payload[slot] = rite;
			RiteDef def = Content.Rites[rite];
			int owned = Vigil.Owned[rite];

			if (!Revealed(rite))
			{
				Ui.SetText(row.Title, "???");
				Ui.SetText(row.Sub, "Something further down. You are not deep enough to name it.");
				Ui.SetText(row.Cost, "");
				Ui.SetText(row.Note, "");
				Ui.SetTextColor(row.Title, Palette.BoneFaint);
				row.Box.SetEnabled(false);
				row.Box.SetColour(Palette.PanelDeep);
				continue;
			}

			int count = BuyAmount > 0 ? BuyAmount : Math.Max(1, Vigil.Affordable(rite));
			double cost = Vigil.CostOfMany(rite, count);
			bool affordable = cost <= Vigil.Ichor;

			Ui.SetText(row.Title, def.Name + (owned > 0 ? "   x" + owned : ""));
			Ui.SetTextColor(row.Title, owned > 0 ? def.Colour : Palette.Bone);
			Ui.SetText(row.Sub, owned > 0
				? Numbers.Rate(owned * def.BaseRate * Vigil.RiteMultiplier(rite)) + "   each " +
				  Numbers.Mult(Vigil.RiteMultiplier(rite)) + "   next double at " +
				  ((owned / Content.MilestoneStep + 1) * Content.MilestoneStep)
				: def.Blurb);
			Ui.SetText(row.Cost, Numbers.Short(cost));
			Ui.SetTextColor(row.Cost, affordable ? Palette.Ichor : Palette.BoneFaint);
			Ui.SetText(row.Note, "buy " + count + (Vigil.Overseers[rite] ? "   overseen" : ""));

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
			_payload[slot] = index;

			bool affordable = def.Cost <= Vigil.Ichor;
			Ui.SetText(row.Title, def.Name);
			Ui.SetTextColor(row.Title, Palette.Bone);
			Ui.SetText(row.Sub, def.Blurb);
			Ui.SetText(row.Cost, Numbers.Short(def.Cost));
			Ui.SetTextColor(row.Cost, affordable ? Palette.Ichor : Palette.BoneFaint);
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
			Ui.SetTextColor(row.Title, Palette.BoneFaint);
			row.Box.SetEnabled(false);
			row.Box.SetColour(Palette.PanelDeep);
			slot = 1;
		}
		BlankFrom(slot);
	}

	// ── Communion ────────────────────────────────────────────────────────────────────

	private void FillCommunion()
	{
		_itemCount = 1 + Content.RiteCount;
		int slot = 0;

		if (_scroll == 0 && slot < _visibleRows)
		{
			int payout = Vigil.SigilsOnOffer;
			bool ready = payout > 0;
			Row row = _rows[slot];
			_payload[slot] = -100;
			Ui.SetText(row.Title, "COMMUNE   +" + payout + " sigils");
			Ui.SetTextColor(row.Title, ready ? Palette.Sigil : Palette.BoneFaint);
			Ui.SetText(row.Sub, ready
				? "Give the parish back. You keep the sigils, the overseers and the marks."
				: "Gather " + Numbers.Short(NextSigilAt()) + " this run for the first sigil.");
			Ui.SetText(row.Cost, Vigil.Sigils + " held");
			Ui.SetTextColor(row.Cost, Palette.Sigil);
			Ui.SetText(row.Note, Numbers.Mult(Vigil.SigilMultiplier) + " from sigils");
			row.Box.SetEnabled(ready);
			row.Box.Style(ready, Palette.Mix(Palette.RowHot, Palette.Sigil, 0.28f), Palette.Row, Palette.PanelDeep);
			if (row.Box.Activated && ready)
			{
				Vigil.Commune();
			}
			slot++;
		}

		for (int rite = Math.Max(0, _scroll - 1); rite < Content.RiteCount && slot < _visibleRows; rite++, slot++)
		{
			Row row = _rows[slot];
			_payload[slot] = rite;
			int cost = Vigil.OverseerCost(rite);
			bool hired = Vigil.Overseers[rite];
			bool affordable = !hired && Vigil.Sigils >= cost;

			Ui.SetText(row.Title, "Overseer: " + Content.Rites[rite].Name);
			Ui.SetTextColor(row.Title, hired ? Content.Rites[rite].Colour : Palette.Bone);
			Ui.SetText(row.Sub, hired
				? "It buys for you, out of surplus, and never touches the ward money."
				: "Hire someone to keep this rite topped up while you are elsewhere.");
			Ui.SetText(row.Cost, hired ? "HIRED" : cost + " sigils");
			Ui.SetTextColor(row.Cost, hired ? Palette.Ichor : affordable ? Palette.Sigil : Palette.BoneFaint);
			Ui.SetText(row.Note, "");
			row.Box.SetEnabled(affordable);
			row.Box.Style(affordable, Palette.RowHot, hired ? Palette.PanelDeep : Palette.Row, Palette.PanelDeep);

			if (row.Box.Activated && affordable)
			{
				Vigil.HireOverseer(rite);
			}
		}
		BlankFrom(slot);
	}

	/// <summary>Run ichor needed for the next whole sigil - the inverse of the payout curve, so
	/// the target shown is the one the button will actually honour.</summary>
	private static double NextSigilAt()
	{
		int next = Vigil.SigilsOnOffer + 1;
		return Math.Pow(next / 8.0, 2.0) * 1e7;
	}

	// ── Marks ────────────────────────────────────────────────────────────────────────

	private void FillMarks()
	{
		_itemCount = Content.Marks.Length + 1;
		int slot = 0;

		if (_scroll == 0 && slot < _visibleRows)
		{
			Row row = _rows[slot];
			Ui.SetText(row.Title, "THE RECORD");
			Ui.SetTextColor(row.Title, Palette.BoneDim);
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
			Ui.SetTextColor(row.Title, earned ? Palette.Ichor : Palette.BoneFaint);
			Ui.SetText(row.Sub, Content.Marks[i].Blurb);
			Ui.SetText(row.Cost, earned ? "KEPT" : "");
			Ui.SetTextColor(row.Cost, Palette.IchorDim);
			Ui.SetText(row.Note, "");
			row.Box.SetEnabled(false);
			row.Box.SetColour(earned ? Palette.Row : Palette.PanelDeep);
		}
		BlankFrom(slot);
	}

	/// <summary>Switch off every pooled row a tab did not use, so a short list cannot leave the
	/// previous tab's text sitting under it.</summary>
	private void BlankFrom(int slot)
	{
		for (int i = slot; i < kRowPool; i++)
		{
			_rows[i].Box.SetActive(false);
		}
	}
}
