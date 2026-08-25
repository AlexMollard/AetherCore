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

	/// <summary>How the game presents itself - and, at the bottom and away from everything else,
	/// the way out of a save.</summary>
	public static readonly string[] SettingsWidgets =
	{
		"ThWhisperLabel", "ThWhisperToggle", "ThShakeLabel", "ThShakeSlider",
		"ThTypeLabel", "ThTypeSlider", "ThWipe",
	};

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

	/// <summary>Whether a named control should be on screen right now.</summary>
	/// <remarks>
	/// The single question the screen asks, once per control, every frame. Everything else here
	/// exists to make this answer true without anybody maintaining a list of what to hide.
	/// </remarks>
	public static bool Shows(string widget)
	{
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
