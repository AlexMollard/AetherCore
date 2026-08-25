using System;

namespace AetherGame;

/// <summary>
/// Everything the parish has said, kept.
/// </summary>
/// <remarks>
/// <para>
/// The whisper feed shows six lines for nine seconds each and then they are gone forever. That
/// is right for the feed - a wall of standing text is not a voice - but it means the one part of
/// the game that carries its atmosphere, teaches the central bargain, and warns what is coming
/// is also the only part a keeper cannot go back and read. Look away for a minute and it is not
/// that you missed a notification; it is that a thing was said to you and there is no record.
/// </para>
/// <para>
/// A ring, so it costs a fixed amount however long a vigil runs, and saved, because "gone
/// forever" was the complaint and a transcript that empties on quit is still gone forever.
/// </para>
/// <para>
/// Engine-free like the rest of the shared vocabulary here, which is also what lets the harness
/// wrap it a thousand times and check it never loses the newest line.
/// </para>
/// </remarks>
public static class Transcript
{
	/// <summary>
	/// How many lines are kept.
	/// </summary>
	/// <remarks>
	/// The feed spends about forty lines a minute at its busiest, so this is a few minutes of
	/// the noisiest play the game can produce and many hours of ordinary play. Larger would be
	/// a save file that grows with playtime, which is the thing a ring buffer exists to avoid.
	/// </remarks>
	public const int Capacity = 160;

	private static readonly string[] s_lines = new string[Capacity];
	private static readonly Omen[] s_omens = new Omen[Capacity];
	private static readonly double[] s_at = new double[Capacity];

	/// <summary>Where the next line goes. Wraps.</summary>
	private static int s_next;

	/// <summary>How many lines are held, never more than <see cref="Capacity"/>.</summary>
	public static int Count { get; private set; }

	/// <summary>
	/// How many lines have ever been said. Not capped, and not the same as <see cref="Count"/>.
	/// </summary>
	/// <remarks>
	/// Only ever read as a DIFFERENCE, by a reader who wants to know how much the list moved
	/// under them since they last looked. See <see cref="Anchored"/>.
	/// </remarks>
	public static long Added { get; private set; }

	/// <summary>Remember a line. Called from the one place the parish speaks.</summary>
	public static void Add(string line, Omen omen, double at)
	{
		if (string.IsNullOrEmpty(line))
		{
			return;
		}
		s_lines[s_next] = line;
		s_omens[s_next] = omen;
		s_at[s_next] = at;
		s_next = (s_next + 1) % Capacity;
		Added++;
		if (Count < Capacity)
		{
			Count++;
		}
	}

	/// <summary>
	/// Where a reader scrolled to <paramref name="scroll"/> should be looking now.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Index 0 is the newest line, so every new line pushes everything the reader is looking at
	/// down by one. Scrolled twenty lines back to work out what took your relic, at forty lines a
	/// minute, the sentence you are halfway through moves off your row about once a second - and
	/// a panel that will not hold still is a panel nobody reads twice.
	/// </para>
	/// <para>
	/// Shifting by the number of lines said since the reader last looked keeps the SAME lines
	/// under them. A reader at the top is left alone, because there the newest line arriving is
	/// the whole point.
	/// </para>
	/// <para>
	/// Here rather than in the panel because it is arithmetic about a ring, which is what this
	/// file is, and because the panel is the one place it could not be checked from.
	/// </para>
	/// </remarks>
	/// <param name="scroll">Which line is at the top of the view.</param>
	/// <param name="seenAdded">What <see cref="Added"/> was when the reader last looked.</param>
	public static int Anchored(int scroll, long seenAdded)
	{
		if (scroll <= 0)
		{
			return 0;
		}
		long moved = Added - seenAdded;
		if (moved <= 0)
		{
			return scroll;
		}
		// Never past the end: once the ring has turned over entirely, the lines the reader was
		// holding are gone and the honest place to put them is the oldest line still kept.
		return (int)Math.Min(scroll + moved, Math.Max(0, Count - 1));
	}

	/// <summary>
	/// One line, newest first.
	/// </summary>
	/// <remarks>
	/// Newest first because that is the order a keeper wants it in: the question a transcript
	/// answers is almost always "what did that just say", not "how did the evening begin".
	/// Index 0 is therefore the most recent thing said, whatever the ring is doing underneath.
	/// </remarks>
	public static (string Line, Omen Omen, double At) At(int index)
	{
		if (index < 0 || index >= Count)
		{
			return ("", Omen.Plain, 0.0);
		}
		int slot = (s_next - 1 - index + Capacity * 2) % Capacity;
		return (s_lines[slot], s_omens[slot], s_at[slot]);
	}

	/// <summary>Forget everything. A new vigil, not a communion - a keeper who communes is the
	/// same keeper and their record should carry over.</summary>
	public static void Clear()
	{
		Array.Clear(s_lines, 0, Capacity);
		Array.Clear(s_omens, 0, Capacity);
		Array.Clear(s_at, 0, Capacity);
		s_next = 0;
		Count = 0;
	}

	/// <summary>
	/// The whole thing, oldest first, for writing to a save.
	/// </summary>
	/// <remarks>
	/// Oldest first rather than newest, so that loading it is a plain replay through
	/// <see cref="Add"/> and the ring ends up in exactly the state it would have reached by
	/// living through the vigil. Reversing on the way out and replaying on the way in is a
	/// smaller thing to get right than persisting the ring's own cursor.
	/// </remarks>
	public static (string[] Lines, int[] Omens, double[] At) Capture()
	{
		string[] lines = new string[Count];
		int[] omens = new int[Count];
		double[] at = new double[Count];
		for (int i = 0; i < Count; i++)
		{
			(string line, Omen omen, double when) = At(Count - 1 - i);
			lines[i] = line;
			omens[i] = (int)omen;
			at[i] = when;
		}
		return (lines, omens, at);
	}

	/// <summary>
	/// Put a saved transcript back, oldest first.
	/// </summary>
	/// <remarks>
	/// Every parameter is NULLABLE in the signature and not merely tolerated in the body. A save
	/// written before one of these arrays existed deserialises it as null, so null is a real
	/// value this is called with rather than a defensive afterthought - and a signature that
	/// claimed otherwise made every honest caller look like a mistake to the compiler.
	/// </remarks>
	public static void Restore(string[]? lines, int[]? omens, double[]? at)
	{
		Clear();
		if (lines == null)
		{
			return;
		}
		for (int i = 0; i < lines.Length; i++)
		{
			int omen = omens != null && i < omens.Length ? omens[i] : 0;
			double when = at != null && i < at.Length ? at[i] : 0.0;
			Add(lines[i], Enum.IsDefined(typeof(Omen), omen) ? (Omen)omen : Omen.Plain, when);
		}
	}
}
