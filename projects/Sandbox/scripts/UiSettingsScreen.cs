using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The Settings screen: look sensitivity, invert-Y, field of view, and a full Controls
/// section - one row per <see cref="SandboxSettings.Bindings"/> entry, each showing its
/// current key as an icon-font glyph (falling back to bracket text for a key this pack has
/// no icon for - see <see cref="InputGlyphs.GetGlyph"/>) and rebindable by clicking it.
/// Shared by two hosts - Main Menu and the in-arena Pause Menu - as two independent
/// instances of this same class, one per scene; see this class's own
/// <see cref="Open"/>/<see cref="IsOpen"/> for how a host talks to it without either side
/// needing to know about the other.
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
/// <b>One row per <see cref="SandboxSettings.Bindings"/> entry, generically.</b> This used
/// to hand-build exactly one row ("Spawn Prop") because it was the only action that
/// actually flowed through <see cref="InputActions"/> - a row that looked rebindable but
/// silently did nothing when rebound would have been worse than no row at all. That
/// blocker is gone: <see cref="SandboxSettings.Bindings"/> is now the single source of
/// truth for every rebindable action, so this loops over it instead of hand-writing one
/// block per row. As of this pass, "spawn_prop" and "open_menu" are consumed live
/// (PropSpawner/SpawnMenu read them through InputActions); "interact"/"tool_fire"/
/// "rotate_prop"/"jump"/"sprint" are registered and rebindable here and persist correctly,
/// but ToolGun/PhysicsGun/FirstPersonPlayer have not yet been migrated off their raw
/// Input.IsKeyDown checks to consume them (a separate, coordinated change owned by
/// whoever's file that migration touches) - rebinding one of those five updates
/// SandboxSettings.BoundKeys and InputActions' own table correctly, it just has no visible
/// gameplay effect until that migration lands. WASD movement is deliberately not offered:
/// it is a continuous two-axis <c>Input.GetAxisRaw</c> read, not a boolean action
/// <see cref="InputActions"/> models, and this sandbox's design never asked for movement
/// to be rebindable away from WASD.
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

    /// <summary>One Controls row: the rebind button (click target + background) and the
    /// icon-font/ordinary-font text pair overlaid on it - two widgets, not one, for the
    /// same reason <see cref="UiHud"/>'s own key-hint pair is two widgets (see
    /// <see cref="InputGlyphs"/>'s own file comment: the icon font is Private-Use-Area
    /// glyphs only and cannot share a string with ordinary text).</summary>
    private readonly struct BindingRow
    {
        public readonly Entity Button;
        public readonly Entity Icon;
        public readonly Entity Text;

        public BindingRow(Entity button, Entity icon, Entity text)
        {
            Button = button;
            Icon = icon;
            Text = text;
        }
    }

    // Shared geometry for every stepper's fill bar/track (see CreateStepper and SetFill).
    private const float FillX = -160.0f;
    private const float FillWidth = 144.0f;
    private const float FillHeight = 6.0f;

    private const float ControlsStartY = 130.0f;
    private const float RowHeight = 36.0f;

    // Never assignable to a rebindable action. Movement (W/A/S/D) is fixed by design (see
    // this class's own remarks); Escape/Tab/the arrows are UiNavigationSystem's own
    // reserved navigation keys - Up/Down/Left/Right/Tab are never consumed by
    // UiNavigationSystem the way Enter/Space are (only Enter/Space call Input::ConsumeKey
    // on activation, confirmed against UiNavigationSystem.cpp, not guessed), so without
    // reserving them here, pressing an arrow key or Tab while "Press a key..." is up would
    // both move this screen's own focus AND get captured as the new binding. Q/E/Space/
    // Shift are deliberately NOT in this list any more - they are now the default keys of
    // OTHER rebindable actions (open_menu/interact-or-rotate_prop/jump/sprint), and
    // whether a candidate collides with one of THOSE is the separate per-action duplicate
    // check in TickCapture, which self-updates as those actions get rebound instead of
    // needing to be hand-maintained here.
    private static readonly Key[] ReservedKeys =
    {
        Key.W, Key.A, Key.S, Key.D, Key.Escape, Key.Up, Key.Down, Key.Left, Key.Right, Key.Tab,
    };

    private Entity _canvas;
    private Entity _panel;
    private Stepper _sensitivity;
    private Stepper _fov;
    private Entity _invertYButton;
    private BindingRow[] _bindingRows = Array.Empty<BindingRow>();
    private Entity _rebindHint;
    private Entity _backButton;

    private bool _capturingRebind;
    private int _rebindIndex = -1;

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

        CreateBindingRows();

        float hintY = ControlsStartY + SandboxSettings.Bindings.Length * RowHeight + 10.0f;
        _rebindHint = Ui.CreateText(_canvas, string.Empty);
        PlaceCentered(_rebindHint, hintY, 320.0f, 24.0f);
        Ui.SetTextAlign(_rebindHint, UiHAlign.Center, UiVAlign.Middle);
        Ui.SetFontSize(_rebindHint, UiTheme.FontSizeHint);
        Ui.SetTextColor(_rebindHint, UiTheme.TextMuted);

        _backButton = Ui.CreateButton(_canvas);
        Ui.SetAnchors(_backButton, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
        Ui.SetPivot(_backButton, new Vector2(0.5f, 0.0f));
        Ui.SetRect(_backButton, 0.0f, hintY + 40.0f, UiTheme.ButtonWidth, UiTheme.ButtonHeight);
        Ui.SetButtonLabel(_backButton, "Back");

        Close();
    }

    /// <summary>Builds one row per <see cref="SandboxSettings.Bindings"/> entry: a label
    /// on the left, a rebind button on the right showing the current key as an icon glyph
    /// (or bracket text where this pack has no icon for it).</summary>
    private void CreateBindingRows()
    {
        _bindingRows = new BindingRow[SandboxSettings.Bindings.Length];
        for (int i = 0; i < SandboxSettings.Bindings.Length; ++i)
        {
            float y = ControlsStartY + i * RowHeight;
            string label = SandboxSettings.Bindings[i].Label;

            Entity labelEntity = Ui.CreateText(_canvas, label);
            PlaceCentered(labelEntity, y, 300.0f, 28.0f, UiHAlign.Left);
            Ui.SetFontSize(labelEntity, UiTheme.FontSizeBody);
            Ui.SetTextColor(labelEntity, UiTheme.TextColor);

            Entity button = Ui.CreateButton(_canvas);
            Ui.SetAnchors(button, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
            Ui.SetPivot(button, new Vector2(0.0f, 1.0f));
            Ui.SetRect(button, 100.0f, y, 100.0f, 32.0f);
            Ui.SetButtonLabel(button, string.Empty); // the icon/text overlay below is the button's visible content

            Entity icon = Ui.CreateText(_canvas, string.Empty);
            Ui.SetAnchors(icon, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
            Ui.SetPivot(icon, new Vector2(0.0f, 1.0f));
            Ui.SetRect(icon, 100.0f, y, 100.0f, 32.0f);
            Ui.SetTextAlign(icon, UiHAlign.Center, UiVAlign.Middle);
            Ui.SetFont(icon, InputGlyphs.FontName);
            Ui.SetFontSize(icon, UiTheme.FontSizeBody);
            Ui.SetTextColor(icon, UiTheme.TextColor);

            Entity text = Ui.CreateText(_canvas, string.Empty);
            Ui.SetAnchors(text, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
            Ui.SetPivot(text, new Vector2(0.0f, 1.0f));
            Ui.SetRect(text, 100.0f, y, 100.0f, 32.0f);
            Ui.SetTextAlign(text, UiHAlign.Center, UiVAlign.Middle);
            Ui.SetFontSize(text, UiTheme.FontSizeBody);
            Ui.SetTextColor(text, UiTheme.TextColor);

            _bindingRows[i] = new BindingRow(button, icon, text);
        }
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
        _rebindIndex = -1;
        _canvas.SetActive(true);
        RefreshSensitivity();
        RefreshFov();
        RefreshInvertY();
        RefreshAllBindings();
        Ui.SetFocus(_sensitivity.Dec);
    }

    private void Close()
    {
        IsOpen = false;
        _capturingRebind = false;
        _rebindIndex = -1;
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
        else
        {
            for (int i = 0; i < _bindingRows.Length; ++i)
            {
                if (!Ui.WasActivated(_bindingRows[i].Button))
                {
                    continue;
                }
                _capturingRebind = true;
                _rebindIndex = i;
                Ui.SetText(_rebindHint, "Press a key... (Esc to cancel)");
                break;
            }
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
            _rebindIndex = -1;
            Ui.SetText(_rebindHint, string.Empty);
            return;
        }

        string action = SandboxSettings.Bindings[_rebindIndex].Action;

        foreach (Key candidate in Enum.GetValues<Key>())
        {
            if (candidate == Key.None || !Input.IsKeyPressed(candidate))
            {
                continue;
            }
            if (Array.IndexOf(ReservedKeys, candidate) >= 0)
            {
                Ui.SetText(_rebindHint, $"{candidate} is reserved for movement/menu navigation - press another key.");
                continue;
            }

            // Self-updating duplicate check: reject a candidate already bound to a
            // DIFFERENT action, so rebinding one action away from its default frees that
            // key up for another - a static reserved-key list could not express that.
            string? conflict = null;
            foreach ((string otherAction, string otherLabel, _) in SandboxSettings.Bindings)
            {
                if (otherAction != action && SandboxSettings.BoundKeys[otherAction] == candidate)
                {
                    conflict = otherLabel;
                    break;
                }
            }
            if (conflict != null)
            {
                Ui.SetText(_rebindHint, $"{candidate} is already bound to {conflict} - press another key.");
                continue;
            }

            SandboxSettings.BoundKeys[action] = candidate;
            InputActions.Register(action, candidate);
            SandboxSettings.Save();
            _capturingRebind = false;
            _rebindIndex = -1;
            RefreshAllBindings();
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

    private void RefreshAllBindings()
    {
        Ui.SetText(_rebindHint, string.Empty);
        for (int i = 0; i < _bindingRows.Length; ++i)
        {
            Key key = SandboxSettings.BoundKeys[SandboxSettings.Bindings[i].Action];
            char? glyph = InputGlyphs.GetGlyph(key);
            if (glyph.HasValue)
            {
                Ui.SetText(_bindingRows[i].Icon, glyph.Value.ToString());
                Ui.SetText(_bindingRows[i].Text, string.Empty);
            }
            else
            {
                Ui.SetText(_bindingRows[i].Icon, string.Empty);
                Ui.SetText(_bindingRows[i].Text, $"[{key}]");
            }
        }
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
