using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// In-game pause menu. Lives on the GameHud canvas (which is DontDestroyOnLoad, so
/// one instance serves every level). Esc/P toggles a freeze - Time.Pause() sets the
/// global time scale to 0, stopping physics, particles and animation while scripts
/// keep ticking - and shows a dim ink overlay with RESUME and ABANDON buttons. Because the
/// script system still runs at dt=0 while frozen, this update keeps reading input to
/// resume and drives the ink materials from Time.UnscaledTime (which keeps advancing while
/// the game clock is stopped). Matches the menu's eerie ink theme. Never pauses during the
/// level-complete beat.
/// </summary>
public sealed class PauseController : EntityScript
{
    private Entity _overlay, _title, _subtitle, _resumeBtn, _resumeLabel, _menuBtn, _menuLabel;
    private bool _paused;

    // Ink palette: buttons rest as dark ink slate and flood cyan on hover (matches the menu toggle).
    private static readonly Vector4 ButtonIdle = new(0.09f, 0.12f, 0.17f, 0.92f);
    private static readonly Vector4 ResumeHover = new(0.16f, 0.42f, 0.52f, 0.96f);
    private static readonly Vector4 MenuHover = new(0.34f, 0.16f, 0.20f, 0.96f); // colder, faintly bloodier for "abandon"
    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 InkEdge = new(0.02f, 0.06f, 0.09f, 1f);

    public override void OnAttach()
    {
        _overlay = Scene.Find("PauseOverlay");
        _title = Scene.Find("PauseTitle");
        _subtitle = Scene.Find("PauseSubtitle");
        _resumeBtn = Scene.Find("PauseResumeBtn");
        _resumeLabel = Scene.Find("PauseResumeLabel");
        _menuBtn = Scene.Find("PauseMenuBtn");
        _menuLabel = Scene.Find("PauseMenuLabel");
        _paused = false;
        Time.Resume();

        // Ink theming: the title glyphs get the glitch-ink material (like the main wordmark) and the
        // overlay + buttons get the brushed pixel-ink material. Applied once; params drive the anim.
        ApplyMaterial(_title, "ui_glitch_text", Cyan);
        ApplyMaterial(_overlay, "ui_ink_ui", InkEdge);
        ApplyMaterial(_resumeBtn, "ui_ink_ui", InkEdge);
        ApplyMaterial(_menuBtn, "ui_ink_ui", InkEdge);

        ShowOverlay(false);
    }

    private static void ApplyMaterial(Entity e, string shader, Vector4 edge)
    {
        if (!e.IsValid) return;
        Ui.SetMaterial(e, shader);
        Ui.SetMaterialColors(e, Vector4.Zero, edge);
    }

    public override void OnUpdate(float deltaTime)
    {
        // While a conversation is open it owns Escape and freezes the game itself; the pause menu
        // stays inert so the two don't fight over input or the time scale.
        if (Dialogue.IsActive)
        {
            return;
        }

        if ((Input.IsKeyPressed(Key.Escape) || Input.IsKeyPressed(Key.P)) && !GameState.Won)
        {
            if (_paused) { Resume(); } else { Pause(); }
        }
        if (!_paused)
        {
            return;
        }

        // Drive the ink materials from the unscaled clock so the glitch/grain keeps breathing while the
        // game clock is frozen (Time.DeltaTime is 0 and Time.TotalTime is stopped during pause).
        Vector4 t = new(Time.UnscaledTime, 0f, 0f, 0f);
        if (_title.IsValid) Ui.SetMaterialParams(_title, t);
        if (_overlay.IsValid) Ui.SetMaterialParams(_overlay, t);
        if (_resumeBtn.IsValid) Ui.SetMaterialParams(_resumeBtn, t);
        if (_menuBtn.IsValid) Ui.SetMaterialParams(_menuBtn, t);

        // Hover highlight + clicks. This runs while frozen because dt=0 frames still tick.
        if (_resumeBtn.IsValid) { Ui.SetImageColor(_resumeBtn, Ui.IsHovered(_resumeBtn) ? ResumeHover : ButtonIdle); }
        if (_menuBtn.IsValid) { Ui.SetImageColor(_menuBtn, Ui.IsHovered(_menuBtn) ? MenuHover : ButtonIdle); }

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
        Log.Info("[INKBOUND] Paused.");
    }

    private void Resume()
    {
        _paused = false;
        Time.Resume();
        ShowOverlay(false);
        Log.Info("[INKBOUND] Resumed.");
    }

    private void ShowOverlay(bool on)
    {
        SetActive(_overlay, on);
        SetActive(_title, on);
        SetActive(_subtitle, on);
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
