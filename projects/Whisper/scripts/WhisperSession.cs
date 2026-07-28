using AetherCore;

namespace AetherGame;

/// <summary>
/// Whisper's session director: everything the SDK's <see cref="NetSessionDirector"/>
/// cannot know, and nothing it already does.
/// </summary>
/// <remarks>
/// <para>
/// The lifecycle itself - spawning a player per connection at a spawn marker, keeping
/// the roster, telling a deliberate ending apart from an accidental one, the bounded
/// reconnect, the on-screen status line, and putting the player back on the menu when it
/// is over - all lives in the SDK, because none of it is about Whisper. What is left
/// here is the four answers only this game has: which prefab, how many players, which
/// scene is the menu, and what the transcript should say.
/// </para>
/// <para>
/// The host is the only peer that announces anything: arrivals and departures are pushed
/// to every transcript through <see cref="ChatBox.Announce"/>, the same multicast the
/// chat itself rides. There is no second mechanism - an announcement is an ordinary chat
/// line the host composed itself.
/// </para>
/// </remarks>
public sealed class WhisperSession : NetSessionDirector
{
    /// <summary>Players in one session, the host included. Whisper's number, not the
    /// framework's: <see cref="Net.Host"/> caps CONNECTIONS, and the host is not one of
    /// its own, so <see cref="ConnectScreen"/> hosts with one fewer than this.</summary>
    public const int MaxPlayers = 4;

    /// <summary>Whisper's answers to the director's questions. In a constructor rather
    /// than field initialisers because they are the BASE class's fields, and because the
    /// script registry caches an instance built this way as the defaults the scene
    /// serializer compares against.</summary>
    public WhisperSession()
    {
        PlayerPrefab = "player";
        SpawnPointCount = MaxPlayers;
        ReturnScene = "Title";
    }

    /// <summary>Escape leaves the arena - unless the chat box has the keyboard, which
    /// uses Escape to cancel an edit. A key that both cancels a message and quits the
    /// session would make the chat unusable.</summary>
    protected override bool WantsToLeave() => !ChatBox.LocalIsTyping && base.WantsToLeave();

    /// <inheritdoc/>
    /// <remarks>
    /// Rides <see cref="NetSessionDirector.LocalPlayer"/> - the host's own player -
    /// because a multicast is refused unless it originates on the host, is addressed on
    /// the wire by the carrier's net id, and needs a carrier that outlives the players it
    /// talks about. <see cref="ChatBox.Announce"/> returns false rather than pretending
    /// whenever the carrier cannot deliver yet, most obviously on the frame the host's
    /// player is spawned, and the director holds the line and asks again.
    /// </remarks>
    protected override bool AnnouncePlayerJoined(NetSessionPlayer player)
        => ChatBox.Announce(LocalPlayer, $"{player.Name} joined");

    /// <inheritdoc/>
    protected override bool AnnouncePlayerLeft(NetSessionPlayer player)
        => ChatBox.Announce(LocalPlayer, $"{player.Name} left");
}
