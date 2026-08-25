namespace AetherGame;

/// <summary>
/// How big the interface's type is.
/// </summary>
/// <remarks>
/// <para>
/// Playtested, everything on screen was hard to read - not one label, the whole interface - so
/// every face is written at its original size and multiplied by <see cref="Scale"/>. One number
/// rather than thirty-odd, because "the UI is too small" is a complaint about all of it at once
/// and answering it by hand-editing forty literals guarantees some of them get missed.
/// </para>
/// <para>
/// <b>The scene generator holds the same number.</b> Most of the game's type is authored into
/// the scene files by <c>tools/generate_scenes.py</c>, which is Python and cannot read a C#
/// constant, so <c>TYPE</c> there and <see cref="Scale"/> here have to be moved together. They
/// were briefly one constant and a set of literals with the scale already baked in, which is
/// worse than this: changing the generator's number then moved every authored label and left
/// the ledger rows and the parish's own labels behind at the old size.
/// </para>
/// <para>
/// Engine-free, like the rest of the shared vocabulary here.
/// </para>
/// </remarks>
public static class Typography
{
	/// <summary>
	/// The multiplier the scene files were BAKED at. Paired with <c>TYPE</c> in
	/// <c>tools/generate_scenes.py</c> - move both or neither.
	/// </summary>
	/// <remarks>
	/// A constant, and it has to stay one: it is not a preference, it is a fact about what is
	/// already written into the authored scenes. Dividing an authored size by it recovers the
	/// size somebody actually typed, which is the only way to re-scale a label at runtime
	/// without keeping a second copy of every authored size in script.
	/// </remarks>
	public const float Authored = 1.30f;

	/// <summary>
	/// Multiplier on every face in the game, as the player has it set.
	/// </summary>
	/// <remarks>
	/// Starts at <see cref="Authored"/>, which is what the scenes already look like, and moves
	/// when a keeper moves the slider. It used to be a constant, so the one complaint a playtest
	/// actually produced - that the whole interface was too small - could only be answered by
	/// editing two numbers in two languages and rebuilding.
	/// </remarks>
	public static float Scale = Authored;

	/// <summary>How far a keeper may take it. Below the floor the ledger's columns stop fitting
	/// their own figures; above the ceiling a 400px column stops holding its own captions.</summary>
	public const float Smallest = 0.85f;

	/// <inheritdoc cref="Smallest"/>
	public const float Largest = 1.75f;

	/// <summary>
	/// A face as it is BUILT, which is at the scale the scenes are authored at.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Authored, not <see cref="Scale"/>, and the difference is a bug that only appeared once
	/// type size became a setting. A script-built row sized its text here and was then scaled
	/// again by <c>TypeScale</c>, which multiplies every label by <c>Scale / Authored</c> - so
	/// the keeper's setting landed on it twice. At the default the two cancel exactly and
	/// everything looks right; at the top of the slider a ledger row came out a third larger
	/// than the authored labels beside it, and at the bottom a third smaller.
	/// </para>
	/// <para>
	/// So this is now the same number the generator bakes, and there is exactly one thing that
	/// applies a keeper's preference: <c>TypeScale.Apply</c>, over everything at once.
	/// </para>
	/// </remarks>
	public static float Face(float size) => size * Authored;

	/// <summary>
	/// A face as it is DRAWN, which is at the size the keeper asked for.
	/// </summary>
	/// <remarks>
	/// For arithmetic about how much room type actually takes on screen - clearances, and
	/// anything that has to not be pushed off the top of a window. Layout is authored at a fixed
	/// scale and the glyphs inside it grow, so what overflows is this and never
	/// <see cref="Face"/>.
	/// </remarks>
	public static float Drawn(float size) => size * Scale;

	/// <summary>
	/// A box that has to hold authored type.
	/// </summary>
	/// <remarks>
	/// <para>
	/// The same multiplier as <see cref="Face"/>, named separately because it is a different
	/// claim: this is a container that was sized around its text and therefore has to match it.
	/// </para>
	/// <para>
	/// <b>Layout is authored at one scale and the glyphs inside it move.</b> That is the model,
	/// and it is why this does not follow the keeper's setting: a box that grew with the type
	/// would have to push everything below it down, which is a re-layout of the whole screen at
	/// runtime rather than a size change. The cost is that the top of the slider is bounded by
	/// how much slack the boxes were drawn with - see <see cref="Largest"/> - and the benefit is
	/// that nothing moves under a keeper who is dragging it.
	/// </para>
	/// </remarks>
	public static float Box(float height) => height * Authored;
}
