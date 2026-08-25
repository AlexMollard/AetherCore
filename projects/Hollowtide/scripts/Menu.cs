using System;

namespace AetherGame;

/// <summary>Which face of the threshold is showing.</summary>
public enum MenuPage
{
	/// <summary>Your name, and the four things you can do.</summary>
	Root,

	/// <summary>Keeping vigil with other people: where to find them, or how to be found.</summary>
	Congregation,

	/// <summary>How the game presents itself, and the way out of a save.</summary>
	Settings,
}

/// <summary>
/// What the threshold shows, and where each control lives.
/// </summary>
/// <remarks>
/// <para>
/// The threshold used to be one flat column with every control on it at once: your name, a host
/// address, four buttons, two settings and the button that erases your save, all competing.
/// A player who only ever keeps vigil alone was asked to look past a networking field to find
/// the button that starts the game, and the one control with no undo behind it sat on the front
/// page.
/// </para>
/// <para>
/// <b>Why this is a table and not a set of SetActive calls.</b> Three pages over twenty controls
/// is sixty decisions, and the failure mode of writing them by hand is a control that is shown
/// on the page it belongs to and never hidden on the other two - which looks completely correct
/// until you open the other page. One table, read in one loop, cannot have that shape of bug: a
/// control is on exactly one page because it appears in exactly one list, and the harness can
/// say so without a window.
/// </para>
/// <para>
/// Engine-free, like the rest of the shared vocabulary here, which is what lets that check exist
/// at all. Selection and focus are deliberately NOT modelled: the engine's widget system already
/// moves a keyboard and a pad around a selectable group, and a second highlight of our own would
/// be a worse copy of a thing that works.
/// </para>
/// </remarks>
public static class Menu
{
	/// <summary>The page showing now.</summary>
	public static MenuPage Page { get; private set; } = MenuPage.Root;

	/// <summary>Chrome: the backdrop, the title, and the lines that report state. On every page,
	/// because they are what the screen IS rather than what it is currently asking.</summary>
	public static readonly string[] Always =
	{
		"ThGloom", "ThTitle", "ThSubtitle", "ThStanding", "ThStatus",
	};

	/// <summary>Who you are, and the four things you can do about it.</summary>
	public static readonly string[] RootWidgets =
	{
		"ThNameLabel", "ThNameBox", "ThPlay", "ThCongregation", "ThSettings", "ThLeave",
	};

	/// <summary>Everything about keeping vigil with somebody else, which a player who never does
	/// now never sees.</summary>
	public static readonly string[] CongregationWidgets =
	{
		"ThAddressLabel", "ThAddressBox", "ThHost", "ThJoin",
	};

	/// <summary>
	/// The settings, named without the screen they are on.
	/// </summary>
	/// <remarks>
	/// There are two settings pages - one on the threshold and one over the vigil - and they show
	/// the same things, because a setting you can only reach from the title screen is a setting
	/// you cannot judge. Type size is the clearest case: the complaint it answers is about the
	/// PARISH's text, and the menu it used to live on does not have any.
	/// <para>
	/// Two pages means two sets of authored controls, which means two chances to add a setting to
	/// one and forget the other. This is the list both are built from, so that cannot happen -
	/// the prefix is the only thing that differs.
	/// </para>
	/// </remarks>
	public static readonly string[] SettingsControls =
	{
		"WhisperLabel", "WhisperToggle", "ShakeLabel", "ShakeSlider", "TypeLabel", "TypeSlider",
	};

	/// <summary>The settings under a screen's own prefix, plus anything only that screen has.</summary>
	public static string[] SettingsFor(string prefix, params string[] extra)
	{
		string[] all = new string[SettingsControls.Length + extra.Length];
		for (int i = 0; i < SettingsControls.Length; i++)
		{
			all[i] = prefix + SettingsControls[i];
		}
		for (int i = 0; i < extra.Length; i++)
		{
			all[SettingsControls.Length + i] = extra[i];
		}
		return all;
	}

	/// <summary>How the game presents itself - and, at the bottom and away from everything else,
	/// the way out of a save. Wiping is the threshold's alone: offering to erase the save from
	/// inside the vigil it is running would be a trapdoor in the middle of the floor.</summary>
	public static readonly string[] SettingsWidgets = SettingsFor("Th", WipeWidget);

	/// <summary>Beginning again. The one control with a condition beyond which page it is on.
	/// </summary>
	public const string WipeWidget = "ThWipe";

	/// <summary>The way back. On every page but the root, which is the page it goes to.</summary>
	public const string BackWidget = "ThBack";

	/// <summary>Every page there is, so a loop over them cannot miss one.</summary>
	public static MenuPage[] Pages => (MenuPage[])Enum.GetValues<MenuPage>().Clone();

	/// <summary>The controls that belong to one page.</summary>
	public static string[] Widgets(MenuPage page) => page switch
	{
		MenuPage.Congregation => CongregationWidgets,
		MenuPage.Settings => SettingsWidgets,
		_ => RootWidgets,
	};

	/// <summary>
	/// Whether a named control should be on screen right now.
	/// </summary>
	/// <remarks>
	/// The single question the screen asks, once per control, every frame. Everything else here
	/// exists to make this answer true without anybody maintaining a list of what to hide.
	/// <para>
	/// <paramref name="hasSave"/> is asked for rather than defaulted, because the one control it
	/// governs is the one that erases a keeper's save. Wiping used to live on the front page and
	/// was hidden outright for somebody with nothing to lose - moving it onto the settings page
	/// put it back in front of a first-time player, offering to delete a save they had not made
	/// yet. A default of "yes there is a save" would have hidden that from this check as neatly
	/// as it hid it from me.
	/// </para>
	/// </remarks>
	public static bool Shows(string widget, bool hasSave)
	{
		if (widget == WipeWidget && !hasSave)
		{
			return false;
		}
		foreach (string always in Always)
		{
			if (always == widget)
			{
				return true;
			}
		}
		if (widget == BackWidget)
		{
			return Page != MenuPage.Root;
		}
		foreach (string here in Widgets(Page))
		{
			if (here == widget)
			{
				return true;
			}
		}
		return false;
	}

	/// <summary>Open a page. Returns whether anything moved, so a caller can reseed focus only
	/// when the screen actually changed under the player.</summary>
	public static bool Open(MenuPage page)
	{
		if (Page == page)
		{
			return false;
		}
		Page = page;
		return true;
	}

	/// <summary>Go back one step, which is always to the root. Returns false at the root, which
	/// is how the screen knows an escape there means something else - there is nowhere further
	/// back to go, and swallowing the key would make the button feel broken.</summary>
	public static bool Back() => Open(MenuPage.Root);

	/// <summary>Start again at the front. Called when the screen is built, so a menu left on the
	/// settings page by a previous session does not greet the next one.</summary>
	public static void Reset() => Page = MenuPage.Root;
}


/// <summary>
/// The menu over the vigil.
/// </summary>
/// <remarks>
/// <para>
/// Escape used to leave the parish outright: one keypress, no confirmation, straight back to the
/// title screen. It saved first, so nothing was lost - but it is still the whole game closing on
/// a key people press to mean "not this".
/// </para>
/// <para>
/// <b>It does not pause.</b> The parish keeps working, dread keeps climbing, and a visitation
/// that arrives while this is open resolves exactly as it would have. That is not an oversight:
/// this game's one promise is that stepping away costs you nothing you were there to defend, and
/// its one bargain is that dread pays and is coming. A menu that stopped the meter would be a
/// button that suspends the bargain while you think, which is the only real way to cheat here.
/// </para>
/// </remarks>
public static class VigilMenu
{
	/// <summary>Whether the menu is up at all. The threshold has no equivalent because the
	/// threshold IS its menu; this one is over something.</summary>
	public static bool Showing { get; private set; }

	/// <summary>Which face of it is showing. Meaningless while closed.</summary>
	public static MenuPage Page { get; private set; } = MenuPage.Root;

	/// <summary>The dimming veil and the heading, up whenever the menu is.</summary>
	public static readonly string[] Always = { "VgMenuVeil", "VgMenuTitle" };

	/// <summary>Three things: go back to it, change how it looks, or leave it.</summary>
	public static readonly string[] RootWidgets = { "VgResume", "VgSettings", "VgLeave" };

	/// <summary>The same settings the threshold offers, under this screen's prefix.</summary>
	public static readonly string[] SettingsWidgets = Menu.SettingsFor("Vg");

	/// <inheritdoc cref="Menu.BackWidget"/>
	public const string BackWidget = "VgBack";

	/// <summary>The controls that belong to one page.</summary>
	public static string[] Widgets(MenuPage page)
		=> page == MenuPage.Settings ? SettingsWidgets : RootWidgets;

	/// <summary>Whether a named control should be on screen right now.</summary>
	public static bool Shows(string widget)
	{
		if (!Showing)
		{
			return false;
		}
		foreach (string always in Always)
		{
			if (always == widget)
			{
				return true;
			}
		}
		if (widget == BackWidget)
		{
			return Page != MenuPage.Root;
		}
		foreach (string here in Widgets(Page))
		{
			if (here == widget)
			{
				return true;
			}
		}
		return false;
	}

	/// <summary>Put the menu up at its root.</summary>
	public static void Show()
	{
		Showing = true;
		Page = MenuPage.Root;
	}

	/// <summary>Take it down.</summary>
	public static void Hide()
	{
		Showing = false;
		Page = MenuPage.Root;
	}

	/// <summary>Open a page. Returns whether anything moved.</summary>
	public static bool Open(MenuPage page)
	{
		if (!Showing || Page == page)
		{
			return false;
		}
		Page = page;
		return true;
	}

	/// <summary>
	/// One step back: out of a page to the root, and out of the root to the parish.
	/// </summary>
	/// <remarks>
	/// Escape is the same key the whole way, and it always means "not this" - so from a settings
	/// page it closes the page rather than the menu, and only at the root does it close the menu.
	/// Anything else makes one press mean two different amounts depending on where you were.
	/// </remarks>
	public static bool Back()
	{
		if (!Showing)
		{
			return false;
		}
		if (Page != MenuPage.Root)
		{
			Page = MenuPage.Root;
			return true;
		}
		Hide();
		return true;
	}
}
