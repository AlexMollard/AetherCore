using AetherCore;

namespace AetherGame;

/// <summary>
/// Hollowtide's answers to the four questions the SDK's session director cannot know:
/// which prefab a keeper is, how many fit, where the menu is, and who owns Escape.
/// </summary>
/// <remarks>
/// Everything else - spawning one keeper per connection at a marker, keeping the roster,
/// telling a deliberate exit apart from a dropped link, the bounded reconnect and the
/// status line - is the framework's, because none of it is about this game.
/// </remarks>
public sealed class HollowtideSession : NetSessionDirector
{
	/// <summary>Keepers in one congregation, the host included. <see cref="Net.Host"/> caps
	/// CONNECTIONS and the host is not one of its own, so the threshold hosts with one fewer.</summary>
	public const int MaxKeepers = Congregation.MaxKeepers;

	/// <summary>Set in a constructor rather than as field initialisers because they are the
	/// BASE class's fields, and the script registry caches an instance built this way as the
	/// defaults the scene serializer compares against.</summary>
	public HollowtideSession()
	{
		PlayerPrefab = "vigil";
		SpawnPointCount = MaxKeepers;
		SpawnSpread = 1.4f;
		ReturnScene = "Threshold";
	}

	/// <summary>Nothing leaves the vigil from here. <see cref="HollowtideGame"/> owns Escape,
	/// and calls <see cref="NetSessionDirector.Leave"/> itself after it has saved - which is
	/// the whole reason to take the key off the base class rather than qualify it: exactly one
	/// reader, and the run is on disk before the session ends.</summary>
	protected override bool WantsToLeave() => false;
}
