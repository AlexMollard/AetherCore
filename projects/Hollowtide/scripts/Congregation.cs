using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The panel down the left: who else is keeping vigil, how they are doing, and the two
/// things you can do to them.
/// </summary>
/// <remarks>
/// <para>
/// Rows are created once and rewritten, never rebuilt on join and leave - a session churns
/// the roster on exactly the frames it is busiest. The row order follows
/// <see cref="VigilPresence.All"/>, which the presence scripts maintain by attach and
/// detach, so this panel keeps nothing of its own that could fall out of step with the
/// lanterns actually standing in the parish.
/// </para>
/// <para>
/// Every action routes through the LOCAL keeper's <see cref="VigilRites"/>, because the
/// host only accepts a request aimed at an entity the sender owns.
/// </para>
/// </remarks>
public sealed class Congregation
{
	public const int MaxKeepers = 4;

	private const float kWidth = 344.0f;
	private const float kRowHeight = 92.0f;
	private const float kRowGap = 8.0f;

	private struct Row
	{
		public Entity Box;
		public Entity Name;
		public Entity Rate;
		public Entity Track;
		public Entity Fill;
		public Entity Ping;
		public Button Tithe;
		public Button Shunt;
		public Button Give;
	}

	private Entity _panel;
	private Entity _heading;
	private readonly Row[] _rows = new Row[MaxKeepers];

	/// <summary>Bind the authored panel and its four rows. The roster is a fixed size - one
	/// row per seat in a congregation - so all of it lives in the scene.</summary>
	public void Bind()
	{
		_panel = Scene.Find("CongregationPanel");
		_heading = Scene.Find("CongHeading");

		for (int i = 0; i < MaxKeepers; i++)
		{
			Row row = default;
			row.Box = Scene.Find("CongRow" + i);
			row.Name = Scene.Find("CongRow" + i + "Name");
			row.Ping = Scene.Find("CongRow" + i + "Ping");
			row.Rate = Scene.Find("CongRow" + i + "Rate");
			row.Track = Scene.Find("CongRow" + i + "Track");
			row.Fill = Scene.Find("CongRow" + i + "Fill");
			row.Tithe = Button.Find("CongRow" + i + "Tithe");
			row.Shunt = Button.Find("CongRow" + i + "Shunt");
			row.Give = Button.Find("CongRow" + i + "Give");
			_rows[i] = row;
		}
	}

	/// <summary>Repaint the roster and act on any button pressed. Returns the number of keepers
	/// present, which the rest of the game uses to size the congregation bonus.</summary>
	public int Update(bool connected)
	{
		// Alone, the panel shows the keepers this one USED to be. Pushing dread onto somebody
		// else is the most distinctive thing this game does, and it needed a second player
		// online - so in the sessions almost everybody actually plays, the panel was hidden and
		// the mechanic did not exist. A communion leaves a keeper behind; they will do.
		if (!connected)
		{
			ShowEchoes();
			return VigilPresence.All.Count;
		}
		_panel.SetActive(true);

		VigilPresence? local = VigilPresence.Local;
		int count = VigilPresence.All.Count;
		Ui.SetText(_heading, count > 1 ? "THE CONGREGATION  " + count : "THE CONGREGATION  alone");

		for (int i = 0; i < MaxKeepers; i++)
		{
			Row row = _rows[i];
			if (i >= count)
			{
				row.Box.SetActive(false);
				continue;
			}
			row.Box.SetActive(true);

			VigilPresence presence = VigilPresence.All[i];
			bool isSelf = presence.IsLocal;
			float dread = Math.Clamp(presence.Dread, 0.0f, 1.0f);

			Ui.SetText(row.Name, presence.Keeper + (isSelf ? "  (you)" : ""));
			Ui.SetTextColor(row.Name, isSelf ? Palette.Ichor : Palette.TextBright);
			Ui.SetText(row.Rate, Numbers.Rate(presence.Rate) + "   " + presence.Sigils + " sigils");
			Ui.SetText(row.Ping, isSelf ? "" : Net.GetPlayerPing(presence.Self) + "ms");

			// The dread bar is two images with the fill resized each frame rather than a
			// progress-bar widget, because it has to sit inside a row that is itself pooled -
			// a widget here would be a component added and removed as the roster changes.
			Vector4 trackRect = Ui.GetRect(row.Track);
			float width = MathF.Max(1.0f, trackRect.Z * dread);
			Ui.SetRect(row.Fill, 0.0f, 0.0f, width, 6.0f);
			Ui.SetImageColor(row.Fill, Palette.Mix(Palette.DreadDeep, new Vector4(0.94f, 0.42f, 0.30f, 1.0f), dread));
			Ui.SetImageColor(row.Box, isSelf ? Palette.RowHot : Palette.Row);

			// You cannot tithe or shunt yourself; the buttons stay visible so the row does not
			// change shape when the roster reorders, and go dead instead.
			bool canAct = !isSelf && local != null;
			double gift = Vigil.Ichor * 0.10;
			bool canTithe = canAct && gift > 0.0;
			bool canShunt = canAct && Vigil.Dread > 0.02;

			// Shown explicitly, because the echo panel hides it and one mode hiding something the
			// other never shows again is how a two-mode panel rots. Net.IsConnected is false for
			// the first frames of EVERY session, hosted ones included, so the echo path always
			// runs first - without this line, tithing was dead in multiplayer from the moment
			// the lone keeper's panel learned to draw ghosts.
			row.Tithe.SetActive(true);
			row.Tithe.SetLabel(canTithe ? "TITHE " + Numbers.Short(gift) : "TITHE");

			// Hands over the FIRST thing in the satchel, and says which. There is no per-relic
			// target picker and there should not be one for three buttons in a 320px row - the
			// satchel's order is on the relics tab where a keeper can already see it, so what
			// this button will give is never a surprise even though it is not chosen here.
			row.Give.SetActive(true);
			bool canGive = canAct && Vigil.Satchel.Count > 0;
			row.Give.SetLabel(canGive
				? Relics.GradeName(Vigil.Satchel[0].Grade).ToUpperInvariant()
				: "NOTHING");
			row.Give.SetEnabled(canGive);
			row.Give.Style(canGive, Palette.Mix(Palette.RowHot, Palette.Ichor, 0.20f),
				Palette.PanelDeep, Palette.PanelDeep);
			if (canGive && row.Give.Activated)
			{
				VigilRites? handing = local!.Self.GetScript<VigilRites>();
				handing?.GiveRelic(presence.Connection, 0);
			}
			row.Tithe.SetEnabled(canTithe);
			row.Tithe.Style(canTithe, Palette.Mix(Palette.RowHot, Palette.Ichor, 0.35f), Palette.PanelDeep, Palette.PanelDeep);

			row.Shunt.SetLabel(canShunt ? "SHUNT " + Numbers.Percent(Math.Min(0.25, Vigil.Dread)) : "SHUNT");
			row.Shunt.SetEnabled(canShunt);
			row.Shunt.Style(canShunt, Palette.Mix(Palette.RowHot, Palette.Dread, 0.45f), Palette.PanelDeep, Palette.PanelDeep);

			if (canTithe && row.Tithe.Activated)
			{
				VigilRites? rites = local!.Self.GetScript<VigilRites>();
				rites?.Tithe(presence.Connection, gift);
			}
			if (canShunt && row.Shunt.Activated)
			{
				VigilRites? rites = local!.Self.GetScript<VigilRites>();
				rites?.Shunt(presence.Connection);
			}
		}

		return count;
	}

	/// <summary>
	/// Draw the line of keepers behind this one, and let the living one lean on them.
	/// </summary>
	/// <remarks>
	/// The same four rows, the same bar, the same button. An echo is not a second system with
	/// its own panel - it is what stands in the empty seats, which is why the dread bar reads
	/// as their BURDEN here and as their exposure when they are alive: in both cases it is what
	/// that keeper is carrying.
	/// </remarks>
	private void ShowEchoes()
	{
		bool any = Vigil.Echoes.Count > 0;
		_panel.SetActive(any);
		if (!any)
		{
			return;
		}

		Ui.SetText(_heading, "THOSE WHO CAME BEFORE  " + Vigil.Echoes.Count);

		for (int i = 0; i < MaxKeepers; i++)
		{
			Row row = _rows[i];
			if (i >= Vigil.Echoes.Count)
			{
				row.Box.SetActive(false);
				continue;
			}
			row.Box.SetActive(true);

			Echo echo = Vigil.Echoes[i];
			float burden = (float)Math.Clamp(echo.Burden / Vigil.kEchoCapacity, 0.0, 1.0);

			// Who they were, not just what they are carrying. A line of four names with four
			// burdens is a table; a line of four keepers you can tell apart is a congregation.
			string was = echo.Rite >= 0
				? "  -  gave it all to the " + Content.Rites[echo.Rite].Name
				: echo.Depth >= 0
					? "  -  never chose, and got as far as the " + Content.Rites[echo.Depth].Name
					: "";
			Ui.SetText(row.Name, echo.Name + was);
			Ui.SetTextColor(row.Name, Palette.TextDim);
			Ui.SetText(row.Rate, burden > 0.01f
				? "carrying " + Numbers.Percent(burden) + " of what you gave them"
				: "carrying nothing yet");
			Ui.SetText(row.Ping, "");

			Vector4 trackRect = Ui.GetRect(row.Track);
			Ui.SetRect(row.Fill, 0.0f, 0.0f, MathF.Max(1.0f, trackRect.Z * burden), 6.0f);
			Ui.SetImageColor(row.Fill, Palette.Mix(Palette.DreadDeep, Palette.Dread, burden));
			Ui.SetImageColor(row.Box, Palette.Row);

			// There is nobody to tithe, and nobody to trade with. The dead have no use for
			// ichor and no hands to take a relic with.
			row.Tithe.SetActive(false);
			row.Give.SetActive(false);

			bool canShunt = Vigil.CanShunt(i);
			row.Shunt.SetLabel(Vigil.ShuntCooldown > 0.0
				? "THEY ARE STILL SETTLING"
				: canShunt
					? "GIVE THEM " + Numbers.Percent(Math.Min(Vigil.kShuntShare, Vigil.Dread))
					: burden >= 1.0 - 1e-6 ? "THEY CAN HOLD NO MORE" : "GIVE THEM YOUR DREAD");
			row.Shunt.SetEnabled(canShunt);
			row.Shunt.Style(canShunt, Palette.Mix(Palette.RowHot, Palette.Dread, 0.45f),
				Palette.PanelDeep, Palette.PanelDeep);

			if (canShunt && row.Shunt.Activated)
			{
				Vigil.ShuntToEcho(i);
			}
		}
	}
}
