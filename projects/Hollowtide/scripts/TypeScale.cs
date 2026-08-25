using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Re-sizes every authored label when a keeper changes the type size.
/// </summary>
/// <remarks>
/// <para>
/// Most of the game's type is written into the scene files by <c>tools/generate_scenes.py</c>,
/// at sizes with <see cref="Typography.Authored"/> already multiplied in. Script-built rows ask
/// <see cref="Typography.Face"/> every frame and so follow the setting on their own; authored
/// labels are set once, by the scene loader, and would otherwise sit at the size the generator
/// chose forever.
/// </para>
/// <para>
/// <b>Adopt once, apply many times.</b> The authored size is read back off each element the
/// first time a canvas is seen and kept, because after the first Apply the element no longer
/// knows what it was authored at - re-reading would compound, and the type would grow every
/// time the slider moved. This is the whole reason the engine needed a font-size GETTER: the
/// alternative is a second copy of every authored size, in script, in a different language from
/// the one that wrote them.
/// </para>
/// <para>
/// Elements that draw no glyphs report zero and are skipped, so a canvas full of images costs
/// one walk and nothing else.
/// </para>
/// </remarks>
public static class TypeScale
{
	private static readonly List<Entity> s_elements = new();
	private static readonly List<float> s_authored = new();

	/// <summary>
	/// Learn what everything under a canvas was authored at. Once per scene, from OnAttach.
	/// </summary>
	/// <remarks>
	/// <para>
	/// <b>Call it exactly once for a scene, and never again while that scene is up.</b> A second
	/// walk would read sizes this has already scaled and record them as what somebody typed, so
	/// the type would grow by the same factor every time - which is why it clears and re-walks
	/// rather than trying to be clever about it.
	/// </para>
	/// <para>
	/// It used to guard against that by remembering the canvas's ENTITY ID and skipping a repeat.
	/// That is the same mistake an index into a satchel is: an id identifies a slot rather than a
	/// thing, and slots are reused. Coming back to the threshold from a vigil whose canvas
	/// happened to hold the same id, the walk was skipped, the list still held the vigil's dead
	/// entities, and type size silently stopped working on the menu that sets it - on some
	/// launches and not others, depending on what the allocator did.
	/// </para>
	/// </remarks>
	public static void Adopt(Entity canvas)
	{
		s_elements.Clear();
		s_authored.Clear();
		if (canvas.IsValid)
		{
			Walk(canvas);
		}
	}

	private static void Walk(Entity e)
	{
		float size = Ui.GetFontSize(e);
		if (size > 0.0f)
		{
			s_elements.Add(e);
			s_authored.Add(size);
		}
		for (int i = 0; i < e.ChildCount; i++)
		{
			Walk(e.GetChild(i));
		}
	}

	/// <summary>Set the type size, and put every adopted label at it.</summary>
	public static void Apply(float scale)
	{
		Typography.Scale = System.Math.Clamp(scale, Typography.Smallest, Typography.Largest);
		for (int i = 0; i < s_elements.Count; i++)
		{
			if (s_elements[i].IsValid)
			{
				// Back to the size somebody typed, then out again to the size that is wanted.
				Ui.SetFontSize(s_elements[i], s_authored[i] / Typography.Authored * Typography.Scale);
			}
		}
	}

	/// <summary>How many labels are being kept in step. Zero before any scene has been adopted,
	/// which is the only thing outside here that can tell whether Adopt found anything.</summary>
	public static int Count => s_elements.Count;
}
