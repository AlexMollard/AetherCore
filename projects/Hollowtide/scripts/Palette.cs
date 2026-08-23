using System.Numerics;

namespace AetherGame;

/// <summary>
/// The whole game's colour, and the rules that govern it.
/// </summary>
/// <remarks>
/// <para>
/// <b>The theme in one line: the parish is drained of colour, and the only colour left in it
/// is what you are harvesting and what is coming for you.</b>
/// </para>
/// <para>
/// So there is exactly ONE grey ramp, cold and desaturated, and everything structural is
/// made of it - panels, rows, buttons, ground, stone, bone, every rite. Nothing structural is
/// ever allowed a hue. Three accents carry saturation and nothing else does: <see cref="Ichor"/>
/// for what you gather, <see cref="Dread"/> for what is closing in, and <see cref="Sigil"/> for
/// what survives a communion. If a pixel is coloured, it means something.
/// </para>
/// <para>
/// This matters because colour was previously invented at four independent sites - a per-rite
/// hue, a greyscale sprite, a light, and a shader - which multiplied into whatever they
/// happened to make. Everything now derives from this file, so the screen cannot drift.
/// </para>
/// <para>
/// Rites are told apart by SILHOUETTE and by WEIGHT, never by hue: a deeper rite is a darker,
/// heavier thing further from the light. That is also why the art is drawn greyscale - it is
/// tinted from <see cref="RiteTint"/>, which only ever returns a step on the ramp.
/// </para>
/// </remarks>
public static class Palette
{
	// Fonts are engine-baked atlases, addressed by name. ASCII only.
	public const string Body = "Roboto-Regular";
	public const string Whisper = "IBMPlexMono-Italic";
	public const string Display = "PixelStorm";

	// ── The ramp. Cold, desaturated, eight steps from the void to the brightest bone. ──
	// Everything structural in the game is one of these and nothing else.
	public static readonly Vector4 Ink = Hex(0x07080B);      // the void behind everything
	public static readonly Vector4 Pitch = Hex(0x0D0F14);    // deepest panel, sunken control
	public static readonly Vector4 Slate = Hex(0x15181F);    // panel
	public static readonly Vector4 Stone = Hex(0x232833);    // row, shadowed structure
	public static readonly Vector4 Ash = Hex(0x3A4150);      // structure in shade
	public static readonly Vector4 Bone = Hex(0x6E7686);     // lit structure, secondary text
	public static readonly Vector4 Pale = Hex(0xA8B0BE);     // primary text
	public static readonly Vector4 Chalk = Hex(0xDCE2EA);    // headings and highlights, used sparingly

	// ── The three accents. Each means one thing. ──
	/// <summary>What you gather. The only green in the game.</summary>
	public static readonly Vector4 Ichor = Hex(0x4ADE9A);
	public static readonly Vector4 IchorDim = Hex(0x1F6B4C);
	/// <summary>What is closing in. The only rust in the game.</summary>
	public static readonly Vector4 Dread = Hex(0xB2453A);
	public static readonly Vector4 DreadDeep = Hex(0x5A1D1B);
	/// <summary>What survives a communion. The only violet in the game.</summary>
	public static readonly Vector4 Sigil = Hex(0x9A86D6);

	public static readonly Vector4 Transparent = new Vector4(0.0f, 0.0f, 0.0f, 0.0f);

	// ── Named roles, so a caller asks for a JOB and not for a colour. ──
	// Anything that wants a colour goes through one of these, which is what stops a new panel
	// inventing a ninth grey that is almost but not quite Slate.
	public static Vector4 Void => Ink;
	public static Vector4 Panel => Fade(Slate, 0.94f);
	public static Vector4 PanelDeep => Fade(Pitch, 0.96f);
	public static Vector4 Row => Fade(Stone, 0.90f);
	public static Vector4 RowHot => Fade(Ash, 0.96f);
	public static Vector4 TextBright => Chalk;
	public static Vector4 TextBody => Pale;
	public static Vector4 TextDim => Bone;
	public static Vector4 TextFaint => Ash;
	/// <summary>The floor. Near-black on purpose: it is lit by every rite standing on it, and
	/// anything brighter came back off the light map as the most prominent thing on screen -
	/// which is exactly wrong for the surface everything else is supposed to sit against.</summary>
	public static Vector4 Ground => Hex(0x090A0E);

	/// <summary>
	/// A rite's tint: a step down the ramp, deepest rite darkest.
	/// </summary>
	/// <remarks>
	/// No hue, deliberately. Eight arbitrary colours made the parish look like a toybox and
	/// told the player nothing; a luminance ramp says the thing the fiction says, which is
	/// that going deeper means going further from the light. They are told apart by their
	/// silhouettes, which is what the silhouettes are for.
	/// </remarks>
	public static Vector4 RiteTint(int rite)
	{
		float t = Content.RiteCount > 1 ? rite / (float)(Content.RiteCount - 1) : 0.0f;
		return Mix(Pale, Ash, t);
	}

	/// <summary>The light a rite casts. Cold and pale like everything else it touches - only
	/// its emissive core is ichor, and that is drawn into the sprite.</summary>
	public static Vector4 RiteLight(int rite) => Mix(Chalk, Bone, rite / (float)Content.RiteCount);

	public static Vector4 Fade(Vector4 colour, float alpha) => new Vector4(colour.X, colour.Y, colour.Z, alpha);

	public static Vector4 Mix(Vector4 a, Vector4 b, float t)
		=> new Vector4(
			a.X + (b.X - a.X) * t,
			a.Y + (b.Y - a.Y) * t,
			a.Z + (b.Z - a.Z) * t,
			a.W + (b.W - a.W) * t);

	/// <summary>0xRRGGBB to a linear-ish Vector4. One place converts, so a colour is written
	/// once as the hex a designer would recognise.</summary>
	private static Vector4 Hex(uint rgb)
		=> new Vector4(
			((rgb >> 16) & 0xFF) / 255.0f,
			((rgb >> 8) & 0xFF) / 255.0f,
			(rgb & 0xFF) / 255.0f,
			1.0f);
}
