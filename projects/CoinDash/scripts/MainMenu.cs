using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen controller. Sits on the Menu scene's UI canvas: pulses the title,
/// gives the PLAY button a hover highlight, and starts the game on click or
/// Enter/Space. Also clears any leftover pause freeze when the menu opens (e.g.
/// after quitting to menu from a paused game).
/// </summary>
public sealed class MainMenu : EntityScript
{
    public string StartScene = "Level1";

    private Entity _play;
    private Entity _title;
    private float _t;

    private static readonly Vector4 Idle = new(0.16f, 0.55f, 0.30f, 0.96f);
    private static readonly Vector4 Hover = new(0.22f, 0.72f, 0.40f, 1.0f);

    public override void OnAttach()
    {
        Time.Resume(); // the menu is never frozen, even if we arrived from a pause
        _play = Scene.Find("PlayButton");
        _title = Scene.Find("MenuTitle");
    }

    public override void OnUpdate(float deltaTime)
    {
        _t += deltaTime;

        // Gentle title breathing so the screen feels alive.
        if (_title.IsValid)
        {
            float pulse = 1.0f + 0.05f * System.MathF.Sin(_t * 2.2f);
            Ui.SetFontSize(_title, 170.0f * pulse);
        }

        bool hover = _play.IsValid && Ui.IsHovered(_play);
        if (_play.IsValid)
        {
            Ui.SetImageColor(_play, hover ? Hover : Idle);
        }

        bool start = (_play.IsValid && Ui.WasClicked(_play))
                     || Input.IsKeyPressed(Key.Enter)
                     || Input.IsKeyPressed(Key.Space);
        if (start)
        {
            Log.Info($"[CoinDash] Starting '{StartScene}'");
            Scene.Load(StartScene);
        }
    }
}
