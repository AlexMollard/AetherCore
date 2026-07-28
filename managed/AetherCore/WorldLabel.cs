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
/// <b>It is outlined by default</b>, because a label over the WORLD has no control over
/// what is behind it. Plain white text is legible over a dark sky and invisible over a
/// pale tile, and which of those a player is standing in front of changes as they walk.
/// The outline is four offset copies of the same string in a dark colour, drawn under the
/// real one - the font pipeline has no stroke of its own, and four extra text elements
/// per label cost far less than the alternative of never being able to read a name.
/// Pass <c>outlineWidth: 0</c> for a label that is always over a surface it controls.
/// </para>
/// <para>
/// Owns its elements: call <see cref="Destroy"/> from the owner's <c>OnDetach</c>, or a
/// despawned character leaves its label floating in the HUD.
/// </para>
/// </remarks>
public sealed class WorldLabel
{
    // The exact sentinel Camera.WorldToScreen returns for a point behind the camera.
    private static readonly Vector2 BehindCameraSentinel = new(-1.0f, -1.0f);

    // Where the four outline copies sit relative to the real text. Diagonals rather than
    // the four cardinal directions: at one pixel the two are indistinguishable, and at
    // two the diagonals close the corners a cardinal cross leaves open.
    private static readonly Vector2[] OutlineOffsets =
    [
        new(-1.0f, -1.0f),
        new(1.0f, -1.0f),
        new(-1.0f, 1.0f),
        new(1.0f, 1.0f),
    ];

    private readonly float _width;
    private readonly float _height;
    private readonly float _outlineWidth;
    private readonly Entity[] _outline;
    private Entity _element;

    /// <summary>
    /// Create the label. Passing no canvas attaches it to the first canvas in the scene,
    /// creating one if the scene has none.
    /// </summary>
    /// <param name="width">Label rect width, in screen pixels.</param>
    /// <param name="height">Label rect height, in screen pixels.</param>
    /// <param name="canvas">Canvas to attach to, or default for the first one.</param>
    /// <param name="outlineWidth">How far the dark backing copies sit from the text, in
    /// pixels. 0 switches the outline off entirely and creates no extra elements.</param>
    /// <param name="outlineColor">Colour of the backing copies. The default is a nearly
    /// opaque black, which is what makes a light label readable over a light background
    /// without turning into a smudge over a dark one.</param>
    public WorldLabel(float width = 160.0f, float height = 24.0f, Entity canvas = default,
        float outlineWidth = 2.0f, Vector4? outlineColor = null)
    {
        _width = width;
        _height = height;
        _outlineWidth = outlineWidth;

        // The backing copies are created FIRST so the real text draws over them: the UI
        // builder emits elements in creation order within a canvas, and an outline drawn
        // last would smother the very string it exists to make readable.
        _outline = outlineWidth > 0.0f ? new Entity[OutlineOffsets.Length] : [];
        Vector4 backing = outlineColor ?? new Vector4(0.0f, 0.0f, 0.0f, 0.85f);
        for (int i = 0; i < _outline.Length; i++)
        {
            _outline[i] = Ui.CreateText(canvas);
            StyleAsLabel(_outline[i], width, height);
            Ui.SetTextColor(_outline[i], backing);
        }

        _element = Ui.CreateText(canvas);
        StyleAsLabel(_element, width, height);
        Ui.SetTextColor(_element, new Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    /// <summary>The underlying UI text entity, for styling it further (font size,
    /// colour). Invalid once <see cref="Destroy"/> has been called.</summary>
    /// <remarks>Styling reached through here applies to the text only, not to its
    /// outline. Use <see cref="SetFont"/> and <see cref="SetFontSize"/> for anything the
    /// outline has to match, or the backing copies will be laid out differently from the
    /// string they sit behind and read as a blur.</remarks>
    public Entity Element => _element;

    /// <summary>Set the text colour. The outline is deliberately untouched: it is what
    /// separates the label from the world, not part of the label's identity.</summary>
    public void SetColor(Vector4 color)
    {
        if (_element.IsValid)
        {
            Ui.SetTextColor(_element, color);
        }
    }

    /// <summary>Set the font on the text and every outline copy together.</summary>
    public void SetFont(string fontName)
    {
        ForEachElement(e => Ui.SetFont(e, fontName));
    }

    /// <summary>Set the glyph size on the text and every outline copy together.</summary>
    public void SetFontSize(float pixelSize)
    {
        ForEachElement(e => Ui.SetFontSize(e, pixelSize));
    }

    /// <summary>Show <paramref name="text"/> at <paramref name="worldPosition"/>, hiding
    /// the label entirely when that point is behind the camera. Call every frame.</summary>
    public void Track(Vector3 worldPosition, string text)
    {
        if (!_element.IsValid)
        {
            return;
        }

        Vector2 screenPos = Camera.WorldToScreen(worldPosition);
        bool behindCamera = screenPos == BehindCameraSentinel;

        Ui.SetText(_element, text);
        _element.SetActive(!behindCamera);
        if (!behindCamera)
        {
            Ui.SetRect(_element, screenPos.X, screenPos.Y, _width, _height);
        }

        for (int i = 0; i < _outline.Length; i++)
        {
            Ui.SetText(_outline[i], text);
            _outline[i].SetActive(!behindCamera);
            if (behindCamera)
            {
                continue;
            }
            Vector2 offset = OutlineOffsets[i] * _outlineWidth;
            Ui.SetRect(_outline[i], screenPos.X + offset.X, screenPos.Y + offset.Y, _width, _height);
        }
    }

    /// <summary>Destroy the elements. Safe to call twice.</summary>
    public void Destroy()
    {
        for (int i = 0; i < _outline.Length; i++)
        {
            if (_outline[i].IsValid)
            {
                _outline[i].Destroy();
            }
            _outline[i] = default;
        }
        if (_element.IsValid)
        {
            _element.Destroy();
        }
        _element = default;
    }

    // Top-left anchor so WorldToScreen's top-left-origin pixels mean what they say, and a
    // centre pivot so Track can point straight at the projected world point instead of at
    // an offset corner.
    private static void StyleAsLabel(Entity element, float width, float height)
    {
        Ui.SetTextAlign(element, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetAnchors(element, Vector2.Zero, Vector2.Zero);
        Ui.SetPivot(element, new Vector2(0.5f, 0.5f));
        Ui.SetRect(element, 0.0f, 0.0f, width, height);
    }

    private void ForEachElement(System.Action<Entity> apply)
    {
        foreach (Entity outline in _outline)
        {
            if (outline.IsValid)
            {
                apply(outline);
            }
        }
        if (_element.IsValid)
        {
            apply(_element);
        }
    }
}
