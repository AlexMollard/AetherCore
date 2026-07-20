using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// In-game pause menu. Lives on the GameHud canvas (which is DontDestroyOnLoad, so
/// one instance serves every level). Esc/P toggles a freeze - Time.Pause() sets the
/// global time scale to 0, stopping physics, particles and animation while scripts
/// keep ticking - and shows a dim overlay with RESUME and MENU buttons. Because the
/// script system still runs at dt=0 while frozen, this update keeps reading input to
/// resume. Never pauses during the level-complete beat.
/// </summary>
public sealed class PauseController : EntityScript
{
    private Entity _overlay, _title, _resumeBtn, _resumeLabel, _menuBtn, _menuLabel;
    private bool _paused;

    private static readonly Vector4 ResumeIdle = new(0.16f, 0.55f, 0.30f, 0.96f);
    private static readonly Vector4 ResumeHover = new(0.22f, 0.72f, 0.40f, 1.0f);
    private static readonly Vector4 MenuIdle = new(0.30f, 0.34f, 0.46f, 0.96f);
    private static readonly Vector4 MenuHover = new(0.42f, 0.48f, 0.62f, 1.0f);

    public override void OnAttach()
    {
        _overlay = Scene.Find("PauseOverlay");
        _title = Scene.Find("PauseTitle");
        _resumeBtn = Scene.Find("PauseResumeBtn");
        _resumeLabel = Scene.Find("PauseResumeLabel");
        _menuBtn = Scene.Find("PauseMenuBtn");
        _menuLabel = Scene.Find("PauseMenuLabel");
        _paused = false;
        Time.Resume();
        ShowOverlay(false);
    }

    public override void OnUpdate(float deltaTime)
    {
        if ((Input.IsKeyPressed(Key.Escape) || Input.IsKeyPressed(Key.P)) && !GameState.Won)
        {
            if (_paused) { Resume(); } else { Pause(); }
        }
        if (!_paused)
        {
            return;
        }

        // Hover highlight + clicks. This runs while frozen because dt=0 frames still tick.
        if (_resumeBtn.IsValid) { Ui.SetImageColor(_resumeBtn, Ui.IsHovered(_resumeBtn) ? ResumeHover : ResumeIdle); }
        if (_menuBtn.IsValid) { Ui.SetImageColor(_menuBtn, Ui.IsHovered(_menuBtn) ? MenuHover : MenuIdle); }

        // Mouse click OR keyboard (Enter = resume, M = quit to menu) - both paths work.
        if ((_resumeBtn.IsValid && Ui.WasClicked(_resumeBtn)) || Input.IsKeyPressed(Key.Enter))
        {
            Resume();
        }
        else if ((_menuBtn.IsValid && Ui.WasClicked(_menuBtn)) || Input.IsKeyPressed(Key.M))
        {
            // Quit to the title. Unfreeze, then tear down this HUD (it is
            // DontDestroyOnLoad, so it would otherwise follow us into the menu and
            // keep showing the pause overlay); the next level re-spawns a fresh one.
            _paused = false;
            Time.Resume();
            ShowOverlay(false);
            Self.Destroy();
            Scene.Load("Menu");
        }
    }

    private void Pause()
    {
        _paused = true;
        Time.Pause();
        ShowOverlay(true);
        Log.Info("[CoinDash] Paused.");
    }

    private void Resume()
    {
        _paused = false;
        Time.Resume();
        ShowOverlay(false);
        Log.Info("[CoinDash] Resumed.");
    }

    private void ShowOverlay(bool on)
    {
        SetActive(_overlay, on);
        SetActive(_title, on);
        SetActive(_resumeBtn, on);
        SetActive(_resumeLabel, on);
        SetActive(_menuBtn, on);
        SetActive(_menuLabel, on);
    }

    private static void SetActive(Entity e, bool on)
    {
        if (e.IsValid)
        {
            e.SetActive(on);
        }
    }
}
