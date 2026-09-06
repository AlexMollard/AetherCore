using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The Settings screen: look sensitivity, invert-Y, field of view, and a Controls section
/// with the one key binding actually rebindable today. Shared by two hosts - Main Menu and
/// the in-arena Pause Menu - as two independent instances of this same class, one per
/// scene; see this class's own <see cref="Open"/>/<see cref="IsOpen"/> for how a host talks
/// to it without either side needing to know about the other.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why buttons instead of sliders.</b> <see cref="Ui"/> has script-side factories for
/// Canvas/Image/Text/Button/TextBox/Effect, but NONE for a slider, toggle or progress bar -
/// those three widget types render themselves (UiWidgetSystem) but can only be
/// scene-authored (confirmed against INKBOUND's own SettingsScreen.cs, which does
/// <c>Scene.Find("SliderMusic")</c> etc. - it finds scene-authored sliders, it never
/// creates one). Every existing Sandbox screen (NetSandboxConnectMenu, SpawnMenu) builds
/// entirely from script with no scene-authored widgets at all; scene-authoring a slider
/// here would be the first screen in this project to break that convention, and would need
/// the same slider entities duplicated in both the Main Menu scene and the arena scene this
/// screen is shared across. A stepper - a value, a fill bar, and two buttons - reads and
/// adjusts the same way and needs nothing but the widget kinds this project already has a
/// factory for.
/// </para>
/// <para>
/// <b>Why the Controls section is one row, not a full keybinding table.</b>
/// <see cref="PropSpawner"/> is the only Sandbox script that reads its key through
/// <see cref="InputActions"/> ("spawn_prop"). Movement, jump, sprint, the spawn-menu
/// toggle and the physgun's rotate-hold are all raw <c>Input.IsKeyDown</c>/<c>IsKeyPressed</c>
/// calls baked directly into <c>FirstPersonPlayer.cs</c>/<c>SpawnMenu.cs</c>/<c>PhysicsGun.cs</c>.
/// A row that looked reboundable but silently did nothing when rebound would be worse than
/// no row at all, so only the one binding that is genuinely live is offered.
/// </para>
/// </remarks>
public sealed class UiSettingsScreen : EntityScript
{
    private readonly struct Stepper
    {
        public readonly Entity Dec;
        public readonly Entity Inc;
        public readonly Entity ValueText;
        public readonly Entity FillBar;
        public readonly float FillY;

        public Stepper(Entity dec, Entity inc, Entity valueText, Entity fillBar, float fillY)
        {
            Dec = dec;
            Inc = inc;
            ValueText = valueText;
            FillBar = fillBar;
            FillY = fillY;
        }
    }

    // Shared geometry for every stepper's fill bar/track (see CreateStepper and SetFill).
    private const float FillX = -160.0f;
    private const float FillWidth = 144.0f;
    private const float FillHeight = 6.0f;

    // Keys already meaningful elsewhere in this project - never offered as a rebind target,
    // so a player can't silently double-bind "spawn_prop" onto, say, W and make every
    // forward step also cycle-spawn a prop.
    // Up/Down/Left/Right/Tab are never consumed by UiNavigationSystem the way Enter/Space
    // are (see its own source - only Enter/Space call Input::ConsumeKey on activation), so
    // without reserving them here, pressing an arrow key or Tab while "Press a key..." is
    // up would both move the settings screen's own focus AND get captured as the new
    // Spawn Prop binding - silently overwriting F with a key every other menu already
    // depends on for navigation. Confirmed against UiNavigationSystem.cpp, not guessed.
    private static readonly Key[] ReservedKeys =
    {
        Key.W, Key.A, Key.S, Key.D, Key.Space, Key.LeftShift, Key.Q, Key.E, Key.Escape,
        Key.Up, Key.Down, Key.Left, Key.Right, Key.Tab,
    };

    private Entity _canvas;
    private Entity _panel;
    private Stepper _sensitivity;
    private Stepper _fov;
    private Entity _invertYButton;
    private Entity _rebindButton;
    private Entity _rebindHint;
    private Entity _backButton;

    private bool _capturingRebind;

    public bool IsOpen { get; private set; }

    public override void OnAttach()
    {
        SandboxSettings.EnsureLoaded();

        _canvas = Ui.CreateCanvas();
        _canvas.MarkTransient(); // runtime UI, never save-worthy - see PhysicsGun.EnsureHud's own comment on why

        _panel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_panel, Vector2.Zero, Vector2.One);
        Ui.SetOffsets(_panel, Vector2.Zero, Vector2.Zero);
        Ui.SetImageColor(_panel, UiTheme.PanelBackground);

        Entity title = Ui.CreateText(_canvas, "SETTINGS");
        PlaceCentered(title, -200.0f, 300.0f, 36.0f);
        Ui.SetTextAlign(title, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(title, UiTheme.FontSizeHeader);
        Ui.SetTextColor(title, UiTheme.TextColor);

        _sensitivity = CreateStepper(-130.0f, "Look Sensitivity");
        _fov = CreateStepper(-40.0f, "Field of View");

        Entity invertLabel = Ui.CreateText(_canvas, "Invert Y Look");
        PlaceCentered(invertLabel, 30.0f, 300.0f, 28.0f, UiHAlign.Left);
        Ui.SetFontSize(invertLabel, UiTheme.FontSizeBody);
        Ui.SetTextColor(invertLabel, UiTheme.TextColor);

        _invertYButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_invertYButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_invertYButton, new Vector2(0.0f, 1.0f));
        Ui.SetRect(_invertYButton, 100.0f, 30.0f, 100.0f, 32.0f);

        Entity controlsLabel = Ui.CreateText(_canvas, "CONTROLS");
        PlaceCentered(controlsLabel, 90.0f, 300.0f, 24.0f);
        Ui.SetTextAlign(controlsLabel, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(controlsLabel, UiTheme.FontSizeHint);
        Ui.SetTextColor(controlsLabel, UiTheme.TextMuted);

        Entity spawnLabel = Ui.CreateText(_canvas, "Spawn Prop");
        PlaceCentered(spawnLabel, 130.0f, 300.0f, 28.0f, UiHAlign.Left);
        Ui.SetFontSize(spawnLabel, UiTheme.FontSizeBody);
        Ui.SetTextColor(spawnLabel, UiTheme.TextColor);

        _rebindButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_rebindButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_rebindButton, new Vector2(0.0f, 1.0f));
        Ui.SetRect(_rebindButton, 100.0f, 130.0f, 100.0f, 32.0f);

        _rebindHint = Ui.CreateText(_canvas, string.Empty);
        PlaceCentered(_rebindHint, 175.0f, 320.0f, 24.0f);
        Ui.SetTextAlign(_rebindHint, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_rebindHint, UiTheme.FontSizeHint);
        Ui.SetTextColor(_rebindHint, UiTheme.TextMuted);

        _backButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_backButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_backButton, new Vector2(0.5f, 0.0f));
        Ui.SetRect(_backButton, 0.0f, 210.0f, UiTheme.ButtonWidth, UiTheme.ButtonHeight);
        Ui.SetButtonLabel(_backButton, "Back");

        Close();
    }

    /// <summary>Builds one "label above, [-]  value  [+] / fill bar below" row. The label
    /// itself is created here rather than passed in as an Entity - callers never need to
    /// touch it again, unlike the stepper's own moving parts.</summary>
    private Stepper CreateStepper(float y, string label)
    {
        Entity labelEntity = Ui.CreateText(_canvas, label);
        PlaceCentered(labelEntity, y, 300.0f, 24.0f, UiHAlign.Left);
        Ui.SetFontSize(labelEntity, UiTheme.FontSizeBody);
        Ui.SetTextColor(labelEntity, UiTheme.TextColor);

        Entity dec = Ui.CreateButton(_canvas);
        Ui.SetAnchors(dec, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(dec, new Vector2(0.0f, 1.0f));
        Ui.SetRect(dec, 160.0f, y, 36.0f, 32.0f);
        Ui.SetButtonLabel(dec, "-");

        Entity valueText = Ui.CreateText(_canvas, string.Empty);
        Ui.SetAnchors(valueText, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(valueText, new Vector2(0.0f, 1.0f));
        Ui.SetRect(valueText, 200.0f, y, 64.0f, 32.0f);
        Ui.SetTextAlign(valueText, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(valueText, UiTheme.FontSizeBody);
        Ui.SetTextColor(valueText, UiTheme.TextColor);

        Entity inc = Ui.CreateButton(_canvas);
        Ui.SetAnchors(inc, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(inc, new Vector2(0.0f, 1.0f));
        Ui.SetRect(inc, 268.0f, y, 36.0f, 32.0f);
        Ui.SetButtonLabel(inc, "+");

        // Below the row it reports on, not above it - a bar with no label in reach until
        // you look down past it read as a disconnected, unlabeled meter on first glance.
        Entity fillTrack = Ui.CreateImage(_canvas);
        Ui.SetAnchors(fillTrack, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(fillTrack, new Vector2(0.0f, 1.0f));
        Ui.SetRect(fillTrack, FillX, y + 14.0f, FillWidth, FillHeight);
        Ui.SetImageColor(fillTrack, UiTheme.TextMuted);

        Entity fillBar = Ui.CreateImage(_canvas);
        Ui.SetAnchors(fillBar, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(fillBar, new Vector2(0.0f, 1.0f));
        Ui.SetImageColor(fillBar, UiTheme.Accent);

        return new Stepper(dec, inc, valueText, fillBar, y + 14.0f);
    }

    private static void PlaceCentered(Entity e, float y, float width, float height, UiHAlign align = UiHAlign.Center)
    {
        Ui.SetAnchors(e, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(e, new Vector2(0.5f, 1.0f));
        Ui.SetRect(e, align == UiHAlign.Center ? 0.0f : -width * 0.5f, y, width, height);
    }

    /// <summary>Shows the screen and seeds every widget from the current
    /// <see cref="SandboxSettings"/> values - called every time a host opens this, not just
    /// once, since the values may have changed on the OTHER host's instance since this one
    /// last showed (Main Menu's Settings and Pause Menu's Settings are two separate scene
    /// instances of this class, and only <see cref="SandboxSettings"/> itself is shared
    /// between them).</summary>
    public void Open()
    {
        IsOpen = true;
        _capturingRebind = false;
        _canvas.SetActive(true);
        RefreshSensitivity();
        RefreshFov();
        RefreshInvertY();
        RefreshRebind();
        Ui.SetFocus(_sensitivity.Dec);
    }

    private void Close()
    {
        IsOpen = false;
        _capturingRebind = false;
        _canvas.SetActive(false);
        Ui.ClearFocus();
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!IsOpen)
        {
            return;
        }

        if (_capturingRebind)
        {
            TickCapture();
            return;
        }

        if (Ui.WasActivated(_backButton))
        {
            Close();
            return;
        }

        if (Ui.WasActivated(_sensitivity.Dec))
        {
            SandboxSettings.LookSensitivity = Math.Clamp(SandboxSettings.LookSensitivity - SandboxSettings.SensitivityStep, SandboxSettings.MinSensitivity, SandboxSettings.MaxSensitivity);
            RefreshSensitivity();
            SandboxSettings.Save();
        }
        else if (Ui.WasActivated(_sensitivity.Inc))
        {
            SandboxSettings.LookSensitivity = Math.Clamp(SandboxSettings.LookSensitivity + SandboxSettings.SensitivityStep, SandboxSettings.MinSensitivity, SandboxSettings.MaxSensitivity);
            RefreshSensitivity();
            SandboxSettings.Save();
        }
        else if (Ui.WasActivated(_fov.Dec))
        {
            SandboxSettings.FovDegrees = Math.Clamp(SandboxSettings.FovDegrees - SandboxSettings.FovStep, SandboxSettings.MinFov, SandboxSettings.MaxFov);
            RefreshFov();
            SandboxSettings.Save();
        }
        else if (Ui.WasActivated(_fov.Inc))
        {
            SandboxSettings.FovDegrees = Math.Clamp(SandboxSettings.FovDegrees + SandboxSettings.FovStep, SandboxSettings.MinFov, SandboxSettings.MaxFov);
            RefreshFov();
            SandboxSettings.Save();
        }
        else if (Ui.WasActivated(_invertYButton))
        {
            SandboxSettings.InvertY = !SandboxSettings.InvertY;
            RefreshInvertY();
            SandboxSettings.Save();
        }
        else if (Ui.WasActivated(_rebindButton))
        {
            _capturingRebind = true;
            Ui.SetText(_rebindHint, "Press a key... (Esc to cancel)");
        }
    }

    /// <summary>Scans every <see cref="Key"/> for one pressed this frame. There is no
    /// first-class "which key was just pressed" API - <see cref="Input"/> only exposes
    /// per-key polling - so this is the workable, if inelegant, alternative: only runs
    /// while this one prompt is open, so a full enum scan per frame costs nothing that
    /// matters.</summary>
    private void TickCapture()
    {
        if (Input.IsKeyPressed(Key.Escape))
        {
            _capturingRebind = false;
            RefreshRebind();
            return;
        }

        foreach (Key candidate in Enum.GetValues<Key>())
        {
            if (candidate == Key.None || !Input.IsKeyPressed(candidate))
            {
                continue;
            }
            if (Array.IndexOf(ReservedKeys, candidate) >= 0)
            {
                Ui.SetText(_rebindHint, $"{candidate} is already used elsewhere - press another key.");
                continue;
            }
            SandboxSettings.SpawnPropKey = candidate;
            InputActions.Register("spawn_prop", candidate);
            SandboxSettings.Save();
            _capturingRebind = false;
            RefreshRebind();
            return;
        }
    }

    private void RefreshSensitivity()
    {
        Ui.SetText(_sensitivity.ValueText, SandboxSettings.LookSensitivity.ToString("0.00"));
        SetFill(_sensitivity, Unit(SandboxSettings.LookSensitivity, SandboxSettings.MinSensitivity, SandboxSettings.MaxSensitivity));
    }

    private void RefreshFov()
    {
        Ui.SetText(_fov.ValueText, SandboxSettings.FovDegrees.ToString("0"));
        SetFill(_fov, Unit(SandboxSettings.FovDegrees, SandboxSettings.MinFov, SandboxSettings.MaxFov));
    }

    private void RefreshInvertY()
    {
        Ui.SetButtonLabel(_invertYButton, SandboxSettings.InvertY ? "ON" : "OFF");
    }

    private void RefreshRebind()
    {
        Ui.SetButtonLabel(_rebindButton, SandboxSettings.SpawnPropKey.ToString());
        Ui.SetText(_rebindHint, string.Empty);
    }

    private static float Unit(float value, float min, float max) => max > min ? Math.Clamp((value - min) / (max - min), 0.0f, 1.0f) : 0.0f;

    /// <summary>Redraws the fill bar's width directly via SetRect rather than adjusting
    /// its offsets incrementally - SetRect fully re-places the box from anchor/pivot each
    /// call (see its own doc comment), so recomputing the whole rect from FillX/FillWidth
    /// every time is the safe option; combining SetOffsets with a box last placed by
    /// SetRect is not a documented composition and is not worth risking on a cosmetic fill
    /// bar.</summary>
    private static void SetFill(Stepper stepper, float unit)
    {
        Ui.SetRect(stepper.FillBar, FillX, stepper.FillY, FillWidth * unit, FillHeight);
    }
}
