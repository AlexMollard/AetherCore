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

    public override void OnAttach()
    {
        ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
        _back = Scene.Find("SettingsBack");
        _music = Scene.Find("SliderMusic");
        _sfx = Scene.Find("SliderSfx");
        _ink = Scene.Find("SliderInkGlow");
        _shake = Scene.Find("ShakeToggle");
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
