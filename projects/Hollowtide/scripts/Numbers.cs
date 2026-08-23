using System;
using System.Globalization;

namespace AetherGame;

/// <summary>
/// Big-number presentation for an idle game. Everything the player sees passes through
/// here, so a number never shows up in two different shapes on two different panels.
/// </summary>
/// <remarks>
/// Values are plain doubles. An idle curve reaches ~1e30 in a long session, which a
/// double carries exactly enough of: the mantissa stops being able to represent whole
/// units long before the exponent runs out, and nobody counting in septillions cares
/// about the ones column. The suffix table stops at 1e33 and hands off to exponent
/// notation rather than inventing names past the point where names stop helping.
/// </remarks>
public static class Numbers
{
	// ASCII only, and deliberately two characters wide from "Qa" on: the font pipeline
	// bakes no other glyph set, and a fixed-width suffix keeps a column of costs aligned
	// without measuring text.
	private static readonly string[] s_suffixes =
	{
		"", "K", "M", "B", "T", "Qa", "Qi", "Sx", "Sp", "Oc", "No", "Dc"
	};

	/// <summary>The usual short form: 1.24K, 8.10M, 3.05e42.</summary>
	public static string Short(double value)
	{
		if (double.IsNaN(value) || double.IsInfinity(value))
		{
			return "-";
		}
		bool negative = value < 0.0;
		value = Math.Abs(value);

		string body;
		if (value < 1000.0)
		{
			// Under a thousand the whole number IS the information - "947" not "0.95K".
			body = value < 10.0 && value % 1.0 != 0.0
				? value.ToString("0.0", CultureInfo.InvariantCulture)
				: Math.Floor(value).ToString("0", CultureInfo.InvariantCulture);
		}
		else
		{
			int tier = (int)Math.Floor(Math.Log10(value) / 3.0);
			if (tier < s_suffixes.Length)
			{
				double scaled = value / Math.Pow(1000.0, tier);
				// Three significant figures, always: 9.99K then 10.4K then 104K, so the
				// column never changes width by more than a character.
				string fmt = scaled < 10.0 ? "0.00" : scaled < 100.0 ? "0.0" : "0";
				body = scaled.ToString(fmt, CultureInfo.InvariantCulture) + s_suffixes[tier];
			}
			else
			{
				body = value.ToString("0.00e+0", CultureInfo.InvariantCulture);
			}
		}
		return negative ? "-" + body : body;
	}

	/// <summary>A rate, per second, for a HUD line.</summary>
	public static string Rate(double perSecond) => Short(perSecond) + "/s";

	/// <summary>A percentage, one decimal, for meters and multipliers.</summary>
	public static string Percent(double fraction01)
		=> (fraction01 * 100.0).ToString("0.0", CultureInfo.InvariantCulture) + "%";

	/// <summary>A multiplier as the player thinks of it: x2.50.</summary>
	public static string Mult(double multiplier)
		=> "x" + multiplier.ToString(multiplier < 10.0 ? "0.00" : "0.0", CultureInfo.InvariantCulture);

	/// <summary>A duration as a clock, for offline gains and cooldowns.</summary>
	public static string Duration(double seconds)
	{
		if (seconds < 0.0)
		{
			seconds = 0.0;
		}
		int total = (int)seconds;
		int hours = total / 3600;
		int minutes = total % 3600 / 60;
		int secs = total % 60;
		return hours > 0
			? string.Format(CultureInfo.InvariantCulture, "{0}h {1:00}m", hours, minutes)
			: minutes > 0
				? string.Format(CultureInfo.InvariantCulture, "{0}m {1:00}s", minutes, secs)
				: string.Format(CultureInfo.InvariantCulture, "{0}s", secs);
	}
}
