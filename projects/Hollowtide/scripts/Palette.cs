using System.Numerics;

namespace AetherGame;

/// <summary>
/// Every colour and font the parish is drawn with, in one place.
/// </summary>
/// <remarks>
/// The palette is deliberately narrow: two darks, one bone, one cold ichor green and one
/// rust. A horror screen goes wrong when it is colourful, and a screen with thirty
/// hand-picked colours in it drifts apart the moment two panels are written a week apart.
/// </remarks>
public static class Palette
{
	// Fonts are engine-baked atlases, addressed by name. ASCII only - the pipeline bakes
	// nothing else, so no glyph outside it may reach a label.
	public const string Body = "Roboto-Regular";
	public const string Whisper = "IBMPlexMono-Italic";
	public const string Display = "PixelStorm";

	public static readonly Vector4 Void = new Vector4(0.020f, 0.024f, 0.030f, 1.000f);
	public static readonly Vector4 Panel = new Vector4(0.043f, 0.050f, 0.060f, 0.941f);
	public static readonly Vector4 PanelDeep = new Vector4(0.027f, 0.032f, 0.040f, 0.960f);
	public static readonly Vector4 Row = new Vector4(0.075f, 0.086f, 0.101f, 0.900f);
	public static readonly Vector4 RowHot = new Vector4(0.125f, 0.145f, 0.160f, 0.960f);

	public static readonly Vector4 Bone = new Vector4(0.855f, 0.851f, 0.816f, 1.0f);
	public static readonly Vector4 BoneDim = new Vector4(0.502f, 0.510f, 0.522f, 1.0f);
	public static readonly Vector4 BoneFaint = new Vector4(0.290f, 0.302f, 0.322f, 1.0f);

	/// <summary>Ichor: cold, slightly sick, never quite green enough to be healthy.</summary>
	public static readonly Vector4 Ichor = new Vector4(0.454f, 0.855f, 0.678f, 1.0f);
	public static readonly Vector4 IchorDim = new Vector4(0.220f, 0.450f, 0.360f, 1.0f);

	/// <summary>Dread: old rust, not fresh blood. Fresh blood reads as an action game.</summary>
	public static readonly Vector4 Dread = new Vector4(0.706f, 0.267f, 0.220f, 1.0f);
	public static readonly Vector4 DreadDeep = new Vector4(0.380f, 0.110f, 0.110f, 1.0f);

	/// <summary>Sigils: the one colour in the game that is not of the parish.</summary>
	public static readonly Vector4 Sigil = new Vector4(0.741f, 0.639f, 0.925f, 1.0f);

	public static readonly Vector4 Transparent = new Vector4(0.0f, 0.0f, 0.0f, 0.0f);

	public static Vector4 Fade(Vector4 colour, float alpha) => new Vector4(colour.X, colour.Y, colour.Z, alpha);

	public static Vector4 Mix(Vector4 a, Vector4 b, float t)
		=> new Vector4(
			a.X + (b.X - a.X) * t,
			a.Y + (b.Y - a.Y) * t,
			a.Z + (b.Z - a.Z) * t,
			a.W + (b.W - a.W) * t);
}
