namespace AetherGame;

/// <summary>
/// Session-wide state that outlives a scene load. The name is chosen on the title
/// screen but is not needed until the arena spawns the player, so it lives here
/// rather than being threaded through the scene transition.
/// </summary>
public static partial class WhisperSession
{
    /// <summary>Name this player typed on the connect screen; "Player" if blank.</summary>
    public static string LocalPlayerName = "Player";
}
