using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Floating name label above a Whisper player, tracking the character on
/// screen and showing that player's replicated display name.
/// </summary>
/// <remarks>
/// <para>
/// The label is plain UI (<see cref="Ui.CreateText"/>), not a world-space
/// sprite: every frame it is repositioned by projecting the player's world
/// position, plus a vertical offset, through <see cref="Camera.WorldToScreen"/>
/// - the same top-left-origin pixel space <see cref="Ui.SetRect"/> expects
/// once the label is anchored to the canvas's top-left corner (see OnAttach;
/// a freshly created text element is centre-anchored by default).
/// </para>
/// <para>
/// <see cref="Net.GetPlayerName"/> is read every frame rather than cached in
/// <see cref="OnAttach"/>. The name is a replicated field written by the peer
/// that OWNS the player, so on every other peer it arrives some time after the
/// entity does - and it can change again later, when a second player with the
/// same name joins and this one steps around it. Caching on attach would leave
/// the tag permanently blank for anyone who joined after this script last read
/// it; re-reading every frame means the tag simply updates the moment the real
/// name lands, same as any other replicated field.
/// </para>
/// </remarks>
public sealed class NameTag : EntityScript
{
    /// <summary>World-space units above the player's origin the label tracks.</summary>
    public float VerticalOffset = 1.4f;

    /// <summary>Label rect size in screen pixels.</summary>
    public float Width = 160.0f;
    public float Height = 24.0f;

    // The exact sentinel Camera.WorldToScreen returns when the projected point
    // is behind the camera - see the doc comment on WorldToScreen.
    private static readonly Vector2 BehindCameraSentinel = new(-1.0f, -1.0f);

    private Entity _label;

    public override void OnAttach()
    {
        // Passing a default (invalid) canvas attaches to the first canvas in
        // the scene, creating one if none exists - the Arena scene has none,
        // so this is what actually makes the canvas.
        _label = Ui.CreateText();
        Ui.SetTextAlign(_label, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetTextColor(_label, new Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        // Ui.CreateText anchors new elements to the canvas CENTRE by default
        // (SetRect's x/y are an offset from the anchor point, not an absolute
        // position). Camera.WorldToScreen returns top-left-origin pixels, so
        // the anchor has to be pinned to the canvas's top-left corner for
        // SetRect(screenPos.X, screenPos.Y, ...) in OnUpdate to land where it
        // says it will instead of being offset by half the screen.
        Ui.SetAnchors(_label, Vector2.Zero, Vector2.Zero);
        // Centre pivot so the SetRect call in OnUpdate can point straight at
        // the projected head position instead of an offset top-left corner.
        Ui.SetPivot(_label, new Vector2(0.5f, 0.5f));
        Ui.SetRect(_label, 0.0f, 0.0f, Width, Height);
    }

    public override void OnUpdate(float deltaTime)
    {
        // See the remarks above: never cached, always re-read so a name that
        // arrives late - or changes - is picked up the frame it lands.
        Ui.SetText(_label, Net.GetPlayerName(Self));

        Vector3 headPos = Self.Position + new Vector3(0.0f, VerticalOffset, 0.0f);
        Vector2 screenPos = Camera.WorldToScreen(headPos);

        // Behind the camera: hide the label rather than letting it stick to
        // whichever screen corner the sentinel would otherwise land on.
        bool behindCamera = screenPos == BehindCameraSentinel;
        _label.SetActive(!behindCamera);
        if (behindCamera)
        {
            return;
        }

        Ui.SetRect(_label, screenPos.X, screenPos.Y, Width, Height);
    }

    public override void OnDetach()
    {
        // A despawned player (e.g. a disconnect) must not leave its tag
        // floating in the HUD - destroy it along with the entity that owned it.
        if (_label.IsValid)
        {
            _label.Destroy();
        }
    }
}
