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
	private static uint s_adopted;

	/// <summary>Learn what everything under a canvas was authored at. Safe to call again for the
	/// same canvas - it only does the walk once, because the second walk would be measuring its
	/// own last result.</summary>
	public static void Adopt(Entity canvas)
	{
		if (!canvas.IsValid || s_adopted == canvas.Id)
		{
			return;
		}
		s_adopted = canvas.Id;
		s_elements.Clear();
		s_authored.Clear();
		Walk(canvas);
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

	/// <summary>Forget a canvas, so the next scene adopts its own. Scene entities do not survive
	/// a load, and a stale list is a list of invalid handles.</summary>
	public static void Forget()
	{
		s_adopted = 0;
		s_elements.Clear();
		s_authored.Clear();
	}
}
