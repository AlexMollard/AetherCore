using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The feed along the bottom of the screen: everything the parish has to say, newest at
/// the bottom, fading out as it ages.
/// </summary>
/// <remarks>
/// <para>
/// A fixed pool of line elements, rewritten in place. The alternative - create an element
/// per message and destroy it when it expires - churns the canvas on exactly the frames
/// the game is busiest (a visitation announces itself while it is also shaking the screen
/// and re-tinting the world), and leaks entities the moment one path forgets to destroy.
/// </para>
/// <para>
/// Colour carries the omen, so a taken rite does not read like an offering bought. Nothing
/// here decides WHAT to say: <see cref="Vigil.Announce"/> and the session layer do, and
/// this only decides how long it lingers.
/// </para>
/// </remarks>
public sealed class Whispers
{
	private const int kLines = 6;
	private const float kLifetime = 9.0f;
	private const float kFadeSeconds = 2.5f;
	private const float kLineHeight = 26.0f;
	private const float kWidth = 720.0f;

	private struct Line
	{
		public string Text;
		public Omen Omen;
		public float Age;
	}

	private readonly List<Line> _lines = new List<Line>();
	private readonly Entity[] _elements = new Entity[kLines];
	private bool _built;

	/// <summary>Build the feed under the given canvas, anchored to the bottom-left so it sits
	/// clear of the ledger on the right and grows upward.</summary>
	/// <summary>Bind the authored feed. Six lines, fixed, so the scene owns where they sit and
	/// this only ever rewrites the string and the colour.</summary>
	public void Bind()
	{
		for (int i = 0; i < kLines; i++)
		{
			_elements[i] = Scene.Find("WhisperLine" + i);
		}
		_built = true;
	}

	public void Say(string text, Omen omen)
	{
		if (string.IsNullOrEmpty(text))
		{
			return;
		}
		_lines.Add(new Line { Text = text, Omen = omen, Age = 0.0f });
		while (_lines.Count > kLines)
		{
			_lines.RemoveAt(0);
		}
	}

	public void Clear() => _lines.Clear();

	/// <summary>Ages the feed on UNSCALED time so it keeps reading while the game is frozen
	/// behind a menu or slowed by a surge - a message is not part of the simulation.</summary>
	public void Update(float unscaledDelta, bool visible)
	{
		if (!_built)
		{
			return;
		}
		for (int i = _lines.Count - 1; i >= 0; i--)
		{
			Line line = _lines[i];
			line.Age += unscaledDelta;
			_lines[i] = line;
			if (line.Age > kLifetime)
			{
				_lines.RemoveAt(i);
			}
		}

		// The newest line goes in the LAST element, so the feed grows upward from the bottom
		// and a steady stream does not make older lines jump around.
		for (int slot = 0; slot < kLines; slot++)
		{
			int index = _lines.Count - kLines + slot;
			Entity e = _elements[slot];
			if (!e.IsValid)
			{
				continue;
			}
			if (!visible || index < 0 || index >= _lines.Count)
			{
				Ui.SetText(e, "");
				continue;
			}
			Line line = _lines[index];
			float remaining = kLifetime - line.Age;
			float alpha = remaining < kFadeSeconds ? remaining / kFadeSeconds : 1.0f;
			Ui.SetText(e, line.Text);
			Ui.SetTextColor(e, Palette.Fade(ColourFor(line.Omen), alpha * 0.92f));
		}
	}

	private static Vector4 ColourFor(Omen omen) => omen switch
	{
		Omen.Good => Palette.Ichor,
		Omen.Dread => Palette.Dread,
		Omen.Taken => Palette.Taken,
		_ => Palette.TextDim,
	};
}
