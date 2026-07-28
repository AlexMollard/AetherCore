using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Floating name label above a Whisper player, showing that player's replicated
/// display name.
/// </summary>
/// <remarks>
/// The tracking, projection and behind-the-camera culling are
/// <see cref="WorldLabel"/>'s, because none of that is about names. What is Whisper's is
/// what the label says and how high above the character it floats.
/// <para>
/// <see cref="Net.GetPlayerName"/> is read every frame rather than cached in
/// <see cref="OnAttach"/>. The name is a replicated field written by the peer that OWNS
/// the player, so on every other peer it arrives some time after the entity does - and
/// it can change again later, when a second player with the same name joins and this one
/// steps around it. Caching on attach would leave the tag permanently blank for anyone
/// who joined after this script last read it.
/// </para>
/// </remarks>
public sealed class NameTag : EntityScript
{
    /// <summary>World-space units above the player's origin the label tracks.</summary>
    public float VerticalOffset = 1.4f;

    /// <summary>Label rect size in screen pixels.</summary>
    public float Width = 160.0f;
    public float Height = 24.0f;

    private WorldLabel? _label;

    /// <inheritdoc/>
    public override void OnAttach() => _label = new WorldLabel(Width, Height);

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
        => _label?.Track(Self.Position + new Vector3(0.0f, VerticalOffset, 0.0f), Net.GetPlayerName(Self));

    /// <inheritdoc/>
    public override void OnDetach()
    {
        // A despawned player (e.g. a disconnect) must not leave its tag floating in the
        // HUD - destroy it along with the entity that owned it.
        _label?.Destroy();
        _label = null;
    }
}
