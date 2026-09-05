using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Floating name label above a Whisper player, showing that player's replicated
/// display name in that player's colour.
/// </summary>
/// <remarks>
/// The tracking, projection, outlining and behind-the-camera culling are
/// <see cref="WorldLabel"/>'s, because none of that is about names. What is Whisper's is
/// what the label says, what colour it is, and how high above the character it floats.
/// <para>
/// <see cref="Net.GetPlayerName"/> is read every frame rather than cached in
/// <see cref="OnAttach"/>. The name is a replicated field written by the peer that OWNS
/// the player, so on every other peer it arrives some time after the entity does - and
/// it can change again later, when a second player with the same name joins and this one
/// steps around it. Caching on attach would leave the tag permanently blank for anyone
/// who joined after this script last read it.
/// </para>
/// <para>
/// The colour comes from <see cref="NetPlayerSync"/> rather than being worked out here,
/// so the tag and the character it belongs to cannot end up different colours - which is
/// the one failure that would make a colour-coded roster worse than none.
/// </para>
/// </remarks>
public sealed class NameTag : EntityScript
{
    /// <summary>World-space units above the player's origin the label tracks.</summary>
    public float VerticalOffset = 1.4f;

    /// <summary>Label rect size in screen pixels.</summary>
    public float Width = 160.0f;
    public float Height = 24.0f;

    /// <summary>Tag text size in pixels.</summary>
    public float FontSize = 17.0f;

    private WorldLabel? _label;
    private int _appliedColor = -1;

    // Last raw name seen and what was drawn for it. The name is replicated from the
    // player's owner and a modified client can put anything in it, so it goes through
    // ChatBox.Sanitize before it reaches the font - but never per frame: the tag is
    // rebuilt on change only, like the colour latch beside it.
    private string _shownName = string.Empty;
    private string _drawnName = string.Empty;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        _label = new WorldLabel(Width, Height);
        // Matches the rest of the project's UI. Set through WorldLabel so the outline
        // copies are laid out identically to the text they sit behind - styled through
        // WorldLabel.Element they would not be, and the tag would read as a blur.
        _label.SetFont("IBMPlexMono-Italic");
        _label.SetFontSize(FontSize);
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (_label is null)
        {
            return;
        }
        // A dead player is invisible and parked on its spawn marker until it comes
        // back, so its tag goes with it - a name floating over nothing is worse than
        // no name. Derived from the same replicated health every peer already has, so
        // the tag and the character agree without either being told.
        bool down = GetScript<PlayerCombat>() is { IsAlive: false };
        string raw = down ? string.Empty : Net.GetPlayerName(Self);
        if (raw != _shownName)
        {
            _shownName = raw;
            _drawnName = ChatBox.Sanitize(raw);
        }
        _label.Track(Self.Position + new Vector3(0.0f, VerticalOffset, 0.0f), _drawnName);

        // Looked up per frame for the same reason the name is: on a client the colour
        // arrives by replication after the entity does. Latched on the index so the
        // per-frame cost is an integer compare.
        if (GetScript<NetPlayerSync>() is { } sync && sync.ColorIndex != _appliedColor)
        {
            _appliedColor = sync.ColorIndex;
            _label.SetColor(sync.Color);
        }
    }

    /// <inheritdoc/>
    public override void OnDetach()
    {
        // A despawned player (e.g. a disconnect) must not leave its tag floating in the
        // HUD - destroy it along with the entity that owned it.
        _label?.Destroy();
        _label = null;
    }
}
