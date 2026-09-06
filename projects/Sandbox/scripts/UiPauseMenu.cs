using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The in-arena pause menu: Escape puts it up (Resume / Settings / Disconnect), and
/// leaving the session is behind the Disconnect item rather than on Escape itself.
/// Attached to the same scene entity as <see cref="NetSandboxSession"/> ("NetSession" in
/// Sandbox.scene.toml) - see <see cref="TickMenu"/> for why Disconnect needs that
/// co-location, mirroring Whisper's own <c>PauseMenu</c>/<c>WhisperSession</c> pairing.
/// </summary>
/// <remarks>
/// <para>
/// <b>This does not pause the world</b>, unlike INKBOUND's single-player
/// <c>PauseController</c> (<c>Time.Pause()</c>). Sandbox is multiplayer: freezing the
/// local simulation would not freeze anyone else's session, so "pause" here means "take
/// the keyboard and release the pointer," exactly Whisper's <c>PauseMenu</c> precedent,
/// not "stop the world."
/// </para>
/// <para>
/// <b>Escape has exactly one owner.</b> <see cref="NetSandboxSession"/> must override
/// <c>WantsToLeave()</c> to return false (see that class's own file comment on this
/// exact hook) so the base <see cref="NetSessionDirector"/>'s own Escape-to-leave never
/// fires once this exists - without that override, Escape would still end the session
/// immediately, racing this screen. Disconnect below calls
/// <see cref="NetSessionDirector.Leave"/> itself, once, from a place a player actually
/// chose it.
/// </para>
/// <para>
/// <b>No <see cref="Net.HasAuthority"/> gate.</b> This lives on a plain scene entity, not
/// a per-connection prefab - every peer that loads the arena is looking at their own
/// private copy of this script, the same reason <see cref="SpawnMenu"/>'s own gate is
/// unnecessary for <em>this</em> class even though it is necessary for the per-camera
/// scripts it suppresses (see <see cref="TickClosed"/>).
/// </para>
/// <para>
/// <b>Required gate on the other side.</b> <c>FirstPersonPlayer</c>, <c>PhysicsGun</c> and
/// <c>PropSpawner</c> each need one more early-return - <c>Scene.Find("NetSession").GetScript
/// &lt;UiPauseMenu&gt;() is {IsOpen: true}</c> - alongside their existing
/// <c>SpawnMenu.IsOpen</c> gate. Releasing the pointer lock alone does not stop them from
/// reading raw WASD/mouse input; without this, a paused player could still walk around and
/// swing the physgun blind while looking at the pause panel.
/// </para>
/// </remarks>
public sealed class UiPauseMenu : EntityScript
{
    private enum Screen { Closed, Menu, Settings }

    private Entity _canvas;
    private Entity _dimOverlay;
    private Entity _panel;
    private Entity _title;
    private Entity _resumeButton;
    private Entity _settingsButton;
    private Entity _disconnectButton;

    private Screen _screen = Screen.Closed;

    public bool IsOpen => _screen != Screen.Closed;

    public override void OnAttach()
    {
        _canvas = Ui.CreateCanvas();
        _canvas.MarkTransient(); // runtime UI, never save-worthy - see PhysicsGun.EnsureHud's own comment on why

        _dimOverlay = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_dimOverlay, Vector2.Zero, Vector2.One);
        Ui.SetOffsets(_dimOverlay, Vector2.Zero, Vector2.Zero);
        Ui.SetImageColor(_dimOverlay, new Vector4(0.0f, 0.0f, 0.0f, 0.55f));

        _panel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_panel, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_panel, new Vector2(0.5f, 0.5f));
        Ui.SetRect(_panel, 0.0f, 0.0f, 340.0f, 260.0f);
        Ui.SetImageColor(_panel, UiTheme.PanelBackground);
        Ui.SetImageCornerRadius(_panel, UiTheme.PanelCornerRadius);

        _title = Ui.CreateText(_canvas, "PAUSED");
        Ui.SetAnchors(_title, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_title, new Vector2(0.5f, 1.0f));
        Ui.SetRect(_title, 0.0f, -90.0f, 260.0f, 32.0f);
        Ui.SetTextAlign(_title, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_title, UiTheme.FontSizeHeader);
        Ui.SetTextColor(_title, UiTheme.TextColor);

        _resumeButton = CreateMenuButton(0, "Resume");
        _settingsButton = CreateMenuButton(1, "Settings");
        _disconnectButton = CreateMenuButton(2, "Disconnect");

        Close();
    }

    private Entity CreateMenuButton(int index, string label)
    {
        Entity button = Ui.CreateButton(_canvas);
        Ui.SetAnchors(button, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(button, new Vector2(0.5f, 1.0f));
        Ui.SetRect(button, 0.0f, -30.0f + index * UiTheme.ButtonRowHeight, UiTheme.ButtonWidth, UiTheme.ButtonHeight);
        Ui.SetButtonLabel(button, label);
        return button;
    }

    public override void OnUpdate(float deltaTime)
    {
        switch (_screen)
        {
            case Screen.Closed:
                TickClosed();
                break;
            case Screen.Menu:
                TickMenu();
                break;
            case Screen.Settings:
                TickSettings();
                break;
        }
    }

    /// <summary>Opens on Escape - unless the spawn menu already owns the keyboard (its own
    /// Q-toggle is a different key, but stacking two open panels needing two different
    /// escape routes is not worth the added state). Camera.Main is guaranteed to be the
    /// LOCAL player's own camera (NetPlayerRig never claims it for a remote player), so
    /// this reaches SpawnMenu without any per-connection wiring of its own.</summary>
    private void TickClosed()
    {
        if (!Input.IsKeyPressed(Key.Escape))
        {
            return;
        }
        if (Camera.Main.IsValid && Camera.Main.GetScript<SpawnMenu>() is { IsOpen: true })
        {
            return;
        }
        Open();
    }

    private void TickMenu()
    {
        if (Input.IsKeyPressed(Key.Escape) || Ui.WasActivated(_resumeButton))
        {
            Close();
            return;
        }
        if (Ui.WasActivated(_settingsButton))
        {
            if (GetScript<UiSettingsScreen>() is { } settings)
            {
                _panel.SetActive(false);
                SetMenuButtonsActive(false);
                _title.SetActive(false);
                settings.Open();
                _screen = Screen.Settings;
            }
            return;
        }
        if (Ui.WasActivated(_disconnectButton))
        {
            // Through the session, not Net.Disconnect directly - leaving is a session
            // decision (hand spawn points back, clear reconnect state, load ReturnScene)
            // and NetSandboxSession already owns all of that. Co-located on "NetSession"
            // specifically so this bare GetScript reaches it.
            if (GetScript<NetSandboxSession>() is { } session)
            {
                Close();
                session.Leave(string.Empty);
            }
            else
            {
                Log.Error("[Sandbox] UiPauseMenu: no NetSandboxSession on this entity, so Disconnect has nothing to leave");
            }
        }
    }

    private void TickSettings()
    {
        UiSettingsScreen? settings = GetScript<UiSettingsScreen>();
        if (settings == null || !settings.IsOpen)
        {
            _screen = Screen.Menu;
            _panel.SetActive(true);
            _title.SetActive(true);
            SetMenuButtonsActive(true);
            Ui.SetFocus(_resumeButton);
        }
    }

    private void Open()
    {
        _screen = Screen.Menu;
        _dimOverlay.SetActive(true);
        _panel.SetActive(true);
        _title.SetActive(true);
        SetMenuButtonsActive(true);
        // Free to click the same frame the menu opens, not one frame later - same
        // reasoning as SpawnMenu.Open()'s own comment. FirstPersonPlayer re-requests the
        // lock every active frame it runs, so it comes back on its own once this closes.
        Input.CursorLockRequested = false;
        Input.OsCursorVisible = true;
        Ui.SetFocus(_resumeButton);
    }

    private void Close()
    {
        _screen = Screen.Closed;
        _dimOverlay.SetActive(false);
        _panel.SetActive(false);
        _title.SetActive(false);
        SetMenuButtonsActive(false);
        Ui.ClearFocus();
    }

    private void SetMenuButtonsActive(bool active)
    {
        _resumeButton.SetActive(active);
        _settingsButton.SetActive(active);
        _disconnectButton.SetActive(active);
    }
}
