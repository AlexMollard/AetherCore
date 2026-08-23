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
	}

	private Entity _panel;
	private Entity _heading;
	private readonly Row[] _rows = new Row[MaxKeepers];

	public void Build(Entity canvas)
	{
		_panel = UiKit.Stretch(canvas, new Vector2(0.0f, 0.0f), new Vector2(0.0f, 1.0f),
			new Vector2(20.0f, Hud.BarHeight + 16.0f), new Vector2(20.0f + kWidth, -220.0f), Palette.Panel, 4.0f);

		_heading = UiKit.Text(_panel, "THE CONGREGATION", 16.0f, 12.0f, 300.0f, 26.0f, 16.0f,
			Palette.BoneDim, UiHAlign.Left, Palette.Display);

		for (int i = 0; i < MaxKeepers; i++)
		{
			float y = 44.0f + i * (kRowHeight + kRowGap);
			Row row = default;
			row.Box = UiKit.Image(_panel, 12.0f, y, kWidth - 24.0f, kRowHeight, Palette.Row, 4.0f);
			row.Name = UiKit.Text(row.Box, "", 12.0f, 8.0f, 220.0f, 22.0f, 16.0f, Palette.Bone);
			row.Ping = UiKit.Text(row.Box, "", kWidth - 130.0f, 8.0f, 90.0f, 24.0f, 15.0f,
				Palette.BoneFaint, UiHAlign.Right);
			row.Rate = UiKit.Text(row.Box, "", 12.0f, 30.0f, 240.0f, 22.0f, 16.0f, Palette.IchorDim);
			row.Track = UiKit.Image(row.Box, 12.0f, 52.0f, kWidth - 48.0f, 6.0f, Palette.PanelDeep, 3.0f);
			row.Fill = UiKit.Image(row.Track, 0.0f, 0.0f, 1.0f, 6.0f, Palette.Dread, 3.0f);
			row.Tithe = UiKit.MakeButton(row.Box, "TITHE", 12.0f, 62.0f, 140.0f, 26.0f, 15.0f, Palette.Display);
			row.Shunt = UiKit.MakeButton(row.Box, "SHUNT", 160.0f, 62.0f, 140.0f, 26.0f, 15.0f, Palette.Display);
			_rows[i] = row;
		}
	}

	/// <summary>Repaint the roster and act on any button pressed. Returns the number of keepers
	/// present, which the rest of the game uses to size the congregation bonus.</summary>
	public int Update(bool connected)
	{
		_panel.SetActive(connected);
		if (!connected)
		{
			return VigilPresence.All.Count;
		}

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
			Ui.SetTextColor(row.Name, isSelf ? Palette.Ichor : Palette.Bone);
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

			row.Tithe.SetLabel(canTithe ? "TITHE " + Numbers.Short(gift) : "TITHE");
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
}
