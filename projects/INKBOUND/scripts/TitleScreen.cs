using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Title screen behaviour. Selection (keyboard + mouse) is handled by the engine's
/// UiNavigationSystem via the labels' UI Selectable components; this script only styles by
/// focus, reacts to activation, and flickers the wordmark.</summary>
public sealed class TitleScreen : EntityScript, IMenuScreen
{
    private float _t;
    private Entity _wordmark;
    private readonly Entity[] _markers = new Entity[4];
    private readonly Entity[] _labels = new Entity[4];

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 UnselLabel = new(0.337f, 0.361f, 0.431f, 1f); // #565c6e
    private static readonly Vector4 UnselMarker = new(0.227f, 0.251f, 0.314f, 1f);
    private static readonly Vector4 WordmarkColor = new(0.933f, 0.945f, 0.969f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<TitleScreen>.Register("TitleRoot", this);
        _wordmark = Scene.Find("TitleWordmark");
        for (int i = 0; i < 4; i++)
        {
            _markers[i] = Scene.Find($"Marker{i}");
            _labels[i] = Scene.Find($"Label{i}");
        }
    }

    public void OnShown()
    {
        if (_labels[0].IsValid) Ui.SetFocus(_labels[0]); // default to 'descend'
    }

    public void HandleInput()
    {
        if (Activated(0)) { Log.Info("[INKBOUND] descend"); Scene.Load("Level1"); }
        else if (Activated(1)) MenuController.Instance?.Go(MenuScreen.LevelSelect);
        else if (Activated(2)) MenuController.Instance?.Go(MenuScreen.Settings);
        else if (Activated(3)) Log.Info("[INKBOUND] release (quit)");
    }

    private bool Activated(int i) => _labels[i].IsValid && Ui.WasActivated(_labels[i]);

    public override void OnUpdate(float dt)
    {
        _t += dt;

        for (int i = 0; i < 4; i++)
        {
            bool on = _labels[i].IsValid && Ui.IsFocused(_labels[i]);
            if (_labels[i].IsValid)
            {
                Ui.SetTextColor(_labels[i], on ? Cyan : UnselLabel);
                Ui.SetFontSize(_labels[i], on ? 34f : 30f);
            }
            if (_markers[i].IsValid)
            {
                if (on)
                {
                    Vector4 c = Cyan;
                    c.W = 0.75f + 0.25f * MathF.Sin(_t * 4.5f); // selected blot breathes
                    Ui.SetImageColor(_markers[i], c);
                }
                else
                {
                    Ui.SetImageColor(_markers[i], UnselMarker);
                }
            }
        }

        if (_wordmark.IsValid)
        {
            float n = Frac(MathF.Sin(_t * 12.9898f) * 43758.5453f);
            float a = n > 0.10f ? (0.9f + 0.1f * MathF.Sin(_t * 1.7f)) : 0.55f;
            Vector4 c = WordmarkColor;
            c.W = a;
            Ui.SetTextColor(_wordmark, c);
        }
    }

    private static float Frac(float v) => v - MathF.Floor(v);
}
