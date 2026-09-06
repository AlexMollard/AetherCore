using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The game's front door: title, Play (the existing Host/Join lobby), Play Solo (skip
/// straight to the arena, offline), Settings, Quit. Built entirely from script, matching
/// every other Sandbox screen (NetSandboxConnectMenu, SpawnMenu) rather than
/// scene-authored widgets - see <see cref="UiSettingsScreen"/>'s own file comment for the
/// one place that convention runs into a real SDK limit (sliders/toggles have no
/// script-side factory at all, unlike Button/Image/Text/TextBox).
/// </summary>
/// <remarks>
/// <para>
/// <b>Today this is the only in-game path into multiplayer.</b> Before this scene existed,
/// <c>ProjectSettings.toml</c>'s <c>startupscene</c> pointed straight at the arena
/// ("Sandbox"), so a published build dropped a player into solo play with no way to reach
/// <c>SandboxMenu</c>'s Host/Join screen at all - repointing <c>startupscene</c> at this
/// scene is what restores that path. This scene does not replace
/// <c>NetSandboxConnectMenu</c>'s connect ceremony; "Play" only ever hands off to it via
/// <see cref="Scene.Load"/>, exactly the way that screen already expects to be reached.
/// </para>
/// <para>
/// No <see cref="Net.HasAuthority"/> gate anywhere here, unlike every per-player script in
/// the arena (FirstPersonPlayer, PhysicsGun, SpawnMenu, PropSpawner): this scene never
/// spawns a session or a player, so there is no ownership to resolve - every peer that
/// ever loads this scene is looking at their own private copy of it, the same reason
/// <c>NetSandboxConnectMenu</c> does not gate on authority either.
/// </para>
/// <para>
/// Settings is a sibling script on this same entity (<see cref="UiSettingsScreen"/>),
/// shared with the in-arena Pause Menu (a second, independent instance of the same
/// class). This script never owns Settings' own Back/Close behaviour - it only opens it
/// and notices, by polling <see cref="UiSettingsScreen.IsOpen"/>, when the screen has
/// closed itself again, so Settings does not need to know which screen opened it.
/// </para>
/// </remarks>
public sealed class UiMainMenu : EntityScript
{
    private enum Screen { Title, Settings }

    private Entity _canvas;
    private Entity _titlePanel;
    private Entity _titleText;
    private Entity _playButton;
    private Entity _playSoloButton;
    private Entity _settingsButton;
    private Entity _quitButton;

    private Screen _screen = Screen.Title;

    public override void OnAttach()
    {
        _canvas = Ui.CreateCanvas();
        _canvas.MarkTransient(); // runtime UI, never save-worthy - see PhysicsGun.EnsureHud's own comment on why

        _titlePanel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_titlePanel, Vector2.Zero, Vector2.One);
        Ui.SetOffsets(_titlePanel, Vector2.Zero, Vector2.Zero);
        Ui.SetImageColor(_titlePanel, UiTheme.PanelBackground);

        _titleText = Ui.CreateText(_canvas, "AETHERCORE SANDBOX");
        Ui.SetAnchors(_titleText, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_titleText, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_titleText, 0.0f, -170.0f, 480.0f, 44.0f);
        Ui.SetTextAlign(_titleText, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_titleText, UiTheme.FontSizeHeader);
        Ui.SetTextColor(_titleText, UiTheme.TextColor);

        _playButton = CreateMenuButton(0, "Play");
        _playSoloButton = CreateMenuButton(1, "Play Solo");
        _settingsButton = CreateMenuButton(2, "Settings");
        _quitButton = CreateMenuButton(3, "Quit");

        // Typing nothing, looking at nothing here - no pointer lock wanted, same
        // reasoning as NetSandboxConnectMenu's own OnAttach.
        Input.CursorLockRequested = false;
        Input.OsCursorVisible = true;
        Ui.SetFocus(_playButton);
    }

    private Entity CreateMenuButton(int index, string label)
    {
        Entity button = Ui.CreateButton(_canvas);
        Ui.SetAnchors(button, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(button, new Vector2(0.5f, 1.0f));
        Ui.SetRect(button, 0.0f, -70.0f + index * UiTheme.ButtonRowHeight, UiTheme.ButtonWidth, UiTheme.ButtonHeight);
        Ui.SetButtonLabel(button, label);
        return button;
    }

    public override void OnUpdate(float deltaTime)
    {
        UiSettingsScreen? settings = GetScript<UiSettingsScreen>();

        if (_screen == Screen.Settings)
        {
            // Settings closes itself (its own Back button); this only notices the
            // transition back rather than owning Close() itself - see this class's
            // own file comment.
            if (settings == null || !settings.IsOpen)
            {
                ShowTitle();
            }
            return;
        }

        if (Ui.WasActivated(_playButton))
        {
            Scene.Load("SandboxMenu");
            return;
        }
        if (Ui.WasActivated(_playSoloButton))
        {
            Scene.Load("Sandbox");
            return;
        }
        if (Ui.WasActivated(_settingsButton))
        {
            if (settings != null)
            {
                SetTitleVisible(false);
                settings.Open();
                _screen = Screen.Settings;
            }
            return;
        }
        if (Ui.WasActivated(_quitButton))
        {
            App.Quit();
        }
    }

    private void ShowTitle()
    {
        _screen = Screen.Title;
        SetTitleVisible(true);
        Ui.SetFocus(_playButton);
    }

    private void SetTitleVisible(bool visible)
    {
        _titlePanel.SetActive(visible);
        _titleText.SetActive(visible);
        _playButton.SetActive(visible);
        _playSoloButton.SetActive(visible);
        _settingsButton.SetActive(visible);
        _quitButton.SetActive(visible);
    }
}
