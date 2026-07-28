using System.Numerics;

namespace AetherCore;

/// <summary>
/// A screen-space text element that follows a point in the world - a name tag over a
/// character, a prompt over a door, a number over a hit.
/// </summary>
/// <remarks>
/// <para>
/// Plain UI rather than a world-space sprite, so the text stays the same size at any
/// distance and reads at the same crispness the rest of the HUD does. The label is
/// re-projected by <see cref="Track"/>, which the owner is expected to call every frame
/// from <c>OnUpdate</c>; nothing here ticks on its own.
/// </para>
/// <para>
/// The awkward parts of doing this by hand are what this exists for. A freshly created
/// element is CENTRE-anchored and <see cref="Ui.SetRect"/>'s x/y is an offset from the
/// anchor, so pixels from <see cref="Camera.WorldToScreen"/> land half a screen out
/// unless the anchor is moved to the top-left corner first. And a point behind the
/// camera projects to a sentinel that would otherwise pin the label to a screen corner
/// instead of hiding it.
/// </para>
/// <para>
/// Owns its element: call <see cref="Destroy"/> from the owner's <c>OnDetach</c>, or a
/// despawned character leaves its label floating in the HUD.
/// </para>
/// </remarks>
public sealed class WorldLabel
{
    // The exact sentinel Camera.WorldToScreen returns for a point behind the camera.
    private static readonly Vector2 BehindCameraSentinel = new(-1.0f, -1.0f);

    private readonly float _width;
    private readonly float _height;
    private Entity _element;

    /// <summary>
    /// Create the label. Passing no canvas attaches it to the first canvas in the scene,
    /// creating one if the scene has none.
    /// </summary>
    /// <param name="width">Label rect width, in screen pixels.</param>
    /// <param name="height">Label rect height, in screen pixels.</param>
    /// <param name="canvas">Canvas to attach to, or default for the first one.</param>
    public WorldLabel(float width = 160.0f, float height = 24.0f, Entity canvas = default)
    {
        _width = width;
        _height = height;
        _element = Ui.CreateText(canvas);
        Ui.SetTextAlign(_element, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetTextColor(_element, new Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        // Top-left anchor so WorldToScreen's top-left-origin pixels mean what they say,
        // and a centre pivot so Track can point straight at the projected world point
        // instead of at an offset corner.
        Ui.SetAnchors(_element, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(_element, new Vector2(0.5f, 0.5f));
        Ui.SetRect(_element, 0.0f, 0.0f, width, height);
    }

    /// <summary>The underlying UI text entity, for styling it further (font size,
    /// colour). Invalid once <see cref="Destroy"/> has been called.</summary>
    public Entity Element => _element;

    /// <summary>Show <paramref name="text"/> at <paramref name="worldPosition"/>, hiding
    /// the label entirely when that point is behind the camera. Call every frame.</summary>
    public void Track(Vector3 worldPosition, string text)
    {
        if (!_element.IsValid)
        {
            return;
        }
        Ui.SetText(_element, text);

        Vector2 screenPos = Camera.WorldToScreen(worldPosition);
        bool behindCamera = screenPos == BehindCameraSentinel;
        _element.SetActive(!behindCamera);
        if (behindCamera)
        {
            return;
        }
        Ui.SetRect(_element, screenPos.X, screenPos.Y, _width, _height);
    }

    /// <summary>Destroy the element. Safe to call twice.</summary>
    public void Destroy()
    {
        if (_element.IsValid)
        {
            _element.Destroy();
        }
        _element = default;
    }
}
