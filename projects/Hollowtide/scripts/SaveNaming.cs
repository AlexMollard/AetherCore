using System;

namespace AetherGame;

/// <summary>
/// What a rescued save is called.
/// </summary>
/// <remarks>
/// <para>
/// Pulled out of <c>SaveSystem</c> so the harness can hold it. The rest of that file needs the
/// engine - a log, and the path to the player's app data - so nothing in it can be checked, and
/// this is the part where being wrong costs the most: it decides the name a keeper's only
/// surviving copy is moved to. An off-by-one that returned a name already in use would delete
/// the very thing the rescue exists to preserve, and the mistake would only ever show up on a
/// machine that had already broken a save twice.
/// </para>
/// <para>
/// Takes a predicate rather than touching the disk, which is what makes it testable at all -
/// and which is also the honest shape, since "is this name taken" is the only thing it needs to
/// know about the world.
/// </para>
/// </remarks>
public static class SaveNaming
{
	/// <summary>The suffix a half-finished write carries. Save writes it and Load recovers from
	/// it, so it is spelled once - two spellings would mean the recovery silently never fired
	/// and nobody would find out until somebody needed it.</summary>
	public const string TempSuffix = ".tmp";

	/// <summary>How many broken copies are kept before the oldest name is reused. A keeper who
	/// has broken a hundred saves has a problem no naming scheme is going to solve.</summary>
	public const int MaxKept = 100;

	/// <summary>
	/// A free name to move an unreadable save to.
	/// </summary>
	/// <param name="path">The save's own path.</param>
	/// <param name="taken">Whether a name is already in use.</param>
	/// <returns>
	/// The first unused name, or the last candidate if every one of them is taken - a rescue
	/// that returns nothing would leave the caller to overwrite the live save, which is worse
	/// than reusing the hundredth name.
	/// </returns>
	public static string Kept(string path, Func<string, bool> taken)
	{
		string first = path + ".broken";
		if (taken == null || !taken(first))
		{
			return first;
		}
		string candidate = first;
		for (int i = 1; i < MaxKept; i++)
		{
			candidate = path + ".broken" + i;
			if (!taken(candidate))
			{
				return candidate;
			}
		}
		return candidate;
	}
}
