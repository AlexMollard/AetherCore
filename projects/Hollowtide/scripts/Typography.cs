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

	/// <summary>A face, at its written size times the scale.</summary>
	public static float Face(float size) => size * Scale;

	/// <summary>
	/// A box that has to hold scaled type.
	/// </summary>
	/// <remarks>
	/// The same multiplier, named separately because it is a different claim: this is a
	/// container that was sized around its text and therefore has to grow with it. A bigger face
	/// left in an unchanged box clips, and clipped text is worse than small text.
	/// </remarks>
	public static float Box(float height) => height * Scale;
}
