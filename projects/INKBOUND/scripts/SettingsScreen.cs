using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Settings ("attune"). Sliders and the toggle are engine UI widgets now: the nav
/// system moves focus, UiWidgetSystem adjusts the focused slider (left/right + mouse drag) and
/// flips the toggle on activation, and each widget draws itself. This script just seeds the
/// widgets from GameSettings, mirrors user changes back, and persists. Ink Glow feeds InkBlock;
/// Music/SFX persist for a future audio system.</summary>
public sealed class SettingsScreen : EntityScript, IMenuScreen
{
    private Entity _back;
    private Entity _music, _sfx, _ink, _shake;
    private float _t;

    // Dark teal ink rim the widgets share, so the sliders/toggle read as brushed ink under the shader.
    private static readonly Vector4 InkEdge = new(0.02f, 0.06f, 0.09f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
        _back = Scene.Find("SettingsBack");
        _music = Scene.Find("SliderMusic");
        _sfx = Scene.Find("SliderSfx");
        _ink = Scene.Find("SliderInkGlow");
        _shake = Scene.Find("ShakeToggle");

        // Give the sliders + toggle a chunky, brushed-ink pixel look via the ui_ink_ui material
        // (applied to their own shapes). The text/back button stay clean.
        foreach (Entity w in new[] { _music, _sfx, _ink, _shake })
        {
            if (w.IsValid)
            {
                Ui.SetMaterial(w, "ui_ink_ui");
                Ui.SetMaterialColors(w, Vector4.Zero, InkEdge);
            }
        }
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        // Drift the ink grain slowly (material params.x = time).
        Vector4 p = new(_t, 0f, 0f, 0f);
        if (_music.IsValid) Ui.SetMaterialParams(_music, p);
        if (_sfx.IsValid) Ui.SetMaterialParams(_sfx, p);
        if (_ink.IsValid) Ui.SetMaterialParams(_ink, p);
        if (_shake.IsValid) Ui.SetMaterialParams(_shake, p);
    }

    public void OnShown()
    {
        if (_music.IsValid) Ui.SetSliderValue(_music, GameSettings.MusicVolume);
        if (_sfx.IsValid) Ui.SetSliderValue(_sfx, GameSettings.SfxVolume);
        if (_ink.IsValid) Ui.SetSliderValue(_ink, GameSettings.InkGlow);
        if (_shake.IsValid) Ui.SetToggle(_shake, GameSettings.ScreenShake);
        if (_music.IsValid) Ui.SetFocus(_music);
    }

    public void HandleInput()
    {
        if (_back.IsValid && Ui.WasActivated(_back)) { MenuController.Instance?.Go(MenuScreen.Title); return; }

        bool dirty = false;
        if (_music.IsValid && Ui.WasChanged(_music)) { GameSettings.MusicVolume = Ui.GetSliderValue(_music); dirty = true; }
        if (_sfx.IsValid && Ui.WasChanged(_sfx)) { GameSettings.SfxVolume = Ui.GetSliderValue(_sfx); dirty = true; }
        if (_ink.IsValid && Ui.WasChanged(_ink)) { GameSettings.InkGlow = Ui.GetSliderValue(_ink); dirty = true; }
        if (_shake.IsValid && Ui.WasChanged(_shake)) { GameSettings.ScreenShake = Ui.GetToggle(_shake); dirty = true; }
        if (dirty) GameSettings.Save();
    }
}
