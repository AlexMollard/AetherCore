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
	/// <summary>
	/// A row under the pointer.
	/// </summary>
	/// <remarks>
	/// Between Stone and Ash rather than at Ash. It WAS Ash - and so was
	/// <see cref="TextFaint"/>, which meant the second line of a row vanished completely the
	/// moment the pointer touched it: the text and the surface it sat on were the same colour,
	/// a contrast ratio of 1.04. Highlighting a row is supposed to make it easier to read, not
	/// impossible.
	/// </remarks>
	public static Vector4 RowHot => Fade(Mix(Stone, Ash, 0.55f), 0.97f);

	// ── Text. All four sit ABOVE every surface on the ramp, which is the rule that keeps them
	// legible. Ash and below are surfaces; Bone and above are text. Nothing may straddle.
	public static Vector4 TextBright => Chalk;
	public static Vector4 TextBody => Pale;
	/// <summary>Secondary text. Between Bone and Pale - still plainly quieter than the body,
	/// but no longer the one step above the brightest surface, which read as unlit.</summary>
	public static Vector4 TextDim => Mix(Bone, Pale, 0.55f);
	/// <summary>
	/// Dread, as TYPE.
	/// </summary>
	/// <remarks>
	/// The accent itself is a dark rust, which is right for a bar, a fill or a shader tint and
	/// marginal for words - 2.25:1 on a highlighted row, which is legible only if you already
	/// know what it says. Lifted toward the body grey until it clears the floor, and no further:
	/// at much more than this it stops reading as rust and starts reading as pink.
	/// <para>
	/// A ROLE, not a ninth colour. <c>Dread</c> stays exactly as it was everywhere it is a
	/// surface, because that is where it works.
	/// </para>
	/// </remarks>
	public static Vector4 DreadText => Mix(Dread, Pale, 0.30f);

	/// <summary>The top of a dread meter: the accent lifted toward the light, so a full bar
	/// reads as hot rather than as more of the same red. Named because it was spelled as a raw
	/// literal in two files - the HUD's meter and the congregation's rows - and two bars showing
	/// one quantity in two colours is the failure this whole file exists to prevent.</summary>
	public static Vector4 DreadHot => Hex(0xF06B4D);

	/// <summary>Something being taken. Between the dread accent and the text ramp, because a
	/// line saying the parish lost something has to be legible as well as alarming - it was its
	/// own raw literal, close to DreadHot without being it, for no reason either could state
	/// against the other.</summary>
	public static Vector4 Taken => Hex(0xE66A5A);

	/// <summary>The quietest text there is. Bone, the first step above every surface. It used
	/// to be Ash, whose own note in the ramp above calls it "structure in shade" - it is a
	/// surface colour, and it was never legible as type.</summary>
	public static Vector4 TextFaint => Bone;
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
