using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Settings ("attune"). The nav system moves focus between the controls (↑/↓);
/// this script adjusts the focused slider on ←/→ (a vertical list has no horizontal neighbour
/// so nav leaves those keys free), flips the shake toggle, positions the fills/orbs from the
/// stored values, and persists. Ink Glow feeds InkBlock; Music/SFX persist for a future audio
/// system.</summary>
public sealed class SettingsScreen : EntityScript, IMenuScreen
{
    private Entity _back;
    private Entity _musicTrack, _musicFill, _musicOrb;
    private Entity _sfxTrack, _sfxFill, _sfxOrb;
    private Entity _inkTrack, _inkFill, _inkOrb;
    private Entity _shakeSocket, _shakeOrb, _shakeValue;
    private float _t;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 OffOrb = new(0.15f, 0.17f, 0.22f, 1f);
    private static readonly Vector4 Muted = new(0.337f, 0.361f, 0.431f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
        _back = Scene.Find("SettingsBack");
        _musicTrack = Scene.Find("SliderMusicTrack"); _musicFill = Scene.Find("SliderMusicFill"); _musicOrb = Scene.Find("SliderMusicOrb");
        _sfxTrack = Scene.Find("SliderSfxTrack"); _sfxFill = Scene.Find("SliderSfxFill"); _sfxOrb = Scene.Find("SliderSfxOrb");
        _inkTrack = Scene.Find("SliderInkGlowTrack"); _inkFill = Scene.Find("SliderInkGlowFill"); _inkOrb = Scene.Find("SliderInkGlowOrb");
        _shakeSocket = Scene.Find("ShakeSocket"); _shakeOrb = Scene.Find("ShakeOrb"); _shakeValue = Scene.Find("ShakeValue");
    }

    public void OnShown()
    {
        if (_musicTrack.IsValid) Ui.SetFocus(_musicTrack);
    }

    public void HandleInput()
    {
        if (_back.IsValid && Ui.WasActivated(_back)) { MenuController.Instance?.Go(MenuScreen.Title); return; }
        if (_shakeSocket.IsValid && Ui.WasActivated(_shakeSocket))
        {
            GameSettings.ScreenShake = !GameSettings.ScreenShake;
            GameSettings.Save();
        }

        float step = 0f;
        if (Input.IsKeyPressed(Key.Right)) step = 0.05f;
        else if (Input.IsKeyPressed(Key.Left)) step = -0.05f;
        if (step != 0f)
        {
            if (_musicTrack.IsValid && Ui.IsFocused(_musicTrack)) { GameSettings.MusicVolume = GameSettings.ClampUnit(GameSettings.MusicVolume + step); GameSettings.Save(); }
            else if (_sfxTrack.IsValid && Ui.IsFocused(_sfxTrack)) { GameSettings.SfxVolume = GameSettings.ClampUnit(GameSettings.SfxVolume + step); GameSettings.Save(); }
            else if (_inkTrack.IsValid && Ui.IsFocused(_inkTrack)) { GameSettings.InkGlow = GameSettings.ClampUnit(GameSettings.InkGlow + step); GameSettings.Save(); }
        }
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        PositionSlider(_musicTrack, _musicFill, _musicOrb, GameSettings.MusicVolume);
        PositionSlider(_sfxTrack, _sfxFill, _sfxOrb, GameSettings.SfxVolume);
        PositionSlider(_inkTrack, _inkFill, _inkOrb, GameSettings.InkGlow);

        bool shakeFocused = _shakeSocket.IsValid && Ui.IsFocused(_shakeSocket);
        if (_shakeOrb.IsValid)
        {
            Vector4 c = GameSettings.ScreenShake ? Cyan : OffOrb;
            if (GameSettings.ScreenShake && shakeFocused) c.W = 0.7f + 0.3f * MathF.Sin(_t * 4.5f);
            Ui.SetImageColor(_shakeOrb, c);
        }
        if (_shakeValue.IsValid)
        {
            Ui.SetText(_shakeValue, GameSettings.ScreenShake ? "FULL" : "OFF");
            Ui.SetTextColor(_shakeValue, GameSettings.ScreenShake ? Cyan : Muted);
        }
        if (_back.IsValid)
            Ui.SetTextColor(_back, Ui.IsFocused(_back) ? Cyan : Muted);
    }

    private void PositionSlider(Entity track, Entity fill, Entity orb, float value)
    {
        if (!track.IsValid || !fill.IsValid || !orb.IsValid) return;
        Vector4 t = Ui.GetRect(track); // x, y, w, h in canvas px
        float innerX = t.X + 2f;
        float innerW = t.Z - 4f;
        float innerY = t.Y + 2f;
        float innerH = t.W - 4f;
        float fw = innerW * value;
        Ui.SetRect(fill, innerX, innerY, MathF.Max(fw, 1f), innerH);

        bool focused = Ui.IsFocused(track);
        float r = focused ? 10f : 8f;
        float ox = innerX + fw;
        float oy = t.Y + t.W * 0.5f;
        Ui.SetRect(orb, ox - r, oy - r, r * 2f, r * 2f);
        Vector4 oc = Cyan;
        if (focused) oc.W = 0.7f + 0.3f * MathF.Sin(_t * 4.5f);
        Ui.SetImageColor(orb, oc);
    }
}
