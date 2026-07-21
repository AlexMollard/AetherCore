using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Title screen behaviour: ink-blot selection over the four menu items, item
/// activation, and the wordmark flicker. Driven by MenuController while Title is active.</summary>
public sealed class TitleScreen : EntityScript, IMenuScreen
{
    private int _sel;
    private float _t;
    private Vector2 _lastMouse;
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
        Apply();
    }

    public void OnShown()
    {
        _sel = 0;
        Apply();
    }

    public void HandleInput()
    {
        if (Input.IsKeyPressed(Key.Down)) { _sel = (_sel + 1) & 3; Apply(); }
        if (Input.IsKeyPressed(Key.Up)) { _sel = (_sel + 3) & 3; Apply(); }

        // Mouse: hovering a menu item selects it (gated on actual mouse movement so it
        // never fights keyboard navigation); clicking an item activates it.
        Vector2 mouse = Input.MousePosition;
        if (mouse != _lastMouse)
        {
            _lastMouse = mouse;
            for (int i = 0; i < 4; i++)
            {
                if (_labels[i].IsValid && Ui.IsHovered(_labels[i]))
                {
                    if (i != _sel) { _sel = i; Apply(); }
                    break;
                }
            }
        }

        bool activate = Input.IsKeyPressed(Key.Enter) || Input.IsKeyPressed(Key.Space);
        for (int i = 0; i < 4; i++)
        {
            if (_labels[i].IsValid && Ui.WasClicked(_labels[i]))
            {
                _sel = i;
                Apply();
                activate = true;
                break;
            }
        }
        if (activate) Activate();
    }

    private void Activate()
    {
        switch (_sel)
        {
            case 0: Log.Info("[INKBOUND] descend"); Scene.Load("Level1"); break;          // descend -> new game
            case 1: MenuController.Instance?.Go(MenuScreen.LevelSelect); break;            // return
            case 2: MenuController.Instance?.Go(MenuScreen.Settings); break;               // attune
            case 3: Log.Info("[INKBOUND] release (quit)"); break;                          // release (quit export is a follow-up)
        }
    }

    private void Apply()
    {
        for (int i = 0; i < 4; i++)
        {
            bool on = i == _sel;
            if (_labels[i].IsValid)
            {
                Ui.SetTextColor(_labels[i], on ? Cyan : UnselLabel);
                Ui.SetFontSize(_labels[i], on ? 34f : 30f);
            }
            if (_markers[i].IsValid)
            {
                Ui.SetImageColor(_markers[i], on ? Cyan : UnselMarker);
            }
        }
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;

        // Wordmark flicker: mostly steady with brief dips (a failing sign).
        if (_wordmark.IsValid)
        {
            float n = Frac(MathF.Sin(_t * 12.9898f) * 43758.5453f);
            float a = n > 0.10f ? (0.9f + 0.1f * MathF.Sin(_t * 1.7f)) : 0.55f;
            Vector4 c = WordmarkColor;
            c.W = a;
            Ui.SetTextColor(_wordmark, c);
        }

        // Selected ink-blot breathes so the cursor reads as "alive".
        if (_markers[_sel].IsValid)
        {
            Vector4 c = Cyan;
            c.W = 0.75f + 0.25f * MathF.Sin(_t * 4.5f);
            Ui.SetImageColor(_markers[_sel], c);
        }
    }

    private static float Frac(float v) => v - MathF.Floor(v);
}
