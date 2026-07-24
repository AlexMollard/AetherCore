using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

public enum MenuScreen { Title, LevelSelect, Settings, SlotSelect }

/// <summary>Uniform interface the controller drives on the active screen.</summary>
public interface IMenuScreen
{
    void OnShown();
    void HandleInput();
}

/// <summary>Per-type registry so the controller can reach each screen's script instance
/// without a native "get script by type" API. Each screen registers itself in OnAttach.</summary>
internal static class ScreenRegistry<T> where T : EntityScript
{
    private static readonly Dictionary<string, T> s_byRoot = new();
    public static void Register(string rootName, T inst) => s_byRoot[rootName] = inst;
    public static T? Get(string rootName) => s_byRoot.TryGetValue(rootName, out var v) ? v : null;
}

/// <summary>Menu state machine: shows exactly one screen root, routes per-frame input to that
/// screen's controller, and handles global Escape (back to Title). Screen changes play an
/// "ink flood" transition - dark ink rises from the bottom to cover, the screen swaps while
/// covered, then the ink drains away - so switches feel on-theme instead of snapping.</summary>
public sealed class MenuController : EntityScript
{
    public static MenuController? Instance;
    public MenuScreen Current { get; private set; } = MenuScreen.Title;

    private Entity _titleRoot, _levelRoot, _settingsRoot, _slotRoot;

    // Ink-flood transition: a custom-shader effect ("ui_ink") whose coverage animates 0..1..0.
    // Created on attach; lives only during Play. The shader owns the liquid look.
    private Entity _inkFx;
    private float _inkTime;
    private enum Phase { Idle, Cover, Reveal }
    private Phase _phase = Phase.Idle;
    private float _tt;
    private MenuScreen _pending;
    // Enabling a screen root queues its script's OnAttach (registration) for the NEXT frame, so the
    // screen may not resolve via ActiveScreen() the same frame it is shown. When that happens we defer
    // OnShown to the next update instead of dropping it - otherwise a first-shown screen skips its
    // per-show setup (label refresh, focus, interactable gating).
    private bool _pendingShow;
    private const float CoverDur = 0.34f, RevealDur = 0.42f;
    private const float NoiseAmp = 0.05f, EdgeWidth = 0.028f; // shader params (uv-space)
    private static readonly Vector4 InkColor = new(0.02f, 0.025f, 0.035f, 1f);
    private static readonly Vector4 EdgeColor = GameSettings.Accent;

    public override void OnAttach()
    {
        Instance = this;
        Time.Resume();
        GameSettings.Load();
        SaveSystem.EnsureLoaded();
        _titleRoot = Scene.Find("TitleRoot");
        _levelRoot = Scene.Find("LevelSelectRoot");
        _settingsRoot = Scene.Find("SettingsRoot");
        _slotRoot = Scene.Find("SlotSelectRoot");
        CreateInk();
        ShowInstant(MenuScreen.Title);
    }

    /// <summary>Switch screens with the ink-flood transition (no-op if already there).</summary>
    public void Go(MenuScreen s)
    {
        if (_phase == Phase.Idle && s == Current) return;
        _pending = s;
        _phase = Phase.Cover;
        _tt = 0f;
    }

    public override void OnUpdate(float dt)
    {
        _inkTime += dt; // keep the ink noise rolling whenever it is on screen

        // A screen shown while its script was not yet registered defers OnShown to here.
        if (_pendingShow)
        {
            IMenuScreen? deferred = ActiveScreen();
            if (deferred != null) { deferred.OnShown(); _pendingShow = false; }
        }

        if (_phase == Phase.Idle)
        {
            if (Current != MenuScreen.Title && Input.IsKeyPressed(Key.Escape)) { Go(MenuScreen.Title); return; }
            ActiveScreen()?.HandleInput();
            return;
        }

        // Mid-transition: drive coverage 0..1..0 and swallow input.
        _tt += dt;
        if (_phase == Phase.Cover)
        {
            float p = Math.Clamp(_tt / CoverDur, 0f, 1f);
            SetInk(EaseOut(p)); // rise fast, settle at full cover
            if (p >= 1f) { ShowInstant(_pending); _phase = Phase.Reveal; _tt = 0f; }
        }
        else // Reveal
        {
            float p = Math.Clamp(_tt / RevealDur, 0f, 1f);
            SetInk(1f - EaseIn(p)); // hold, then drain away
            if (p >= 1f) { SetInk(0f); _phase = Phase.Idle; }
        }
    }

    private void ShowInstant(MenuScreen s)
    {
        Current = s;
        if (_titleRoot.IsValid) _titleRoot.SetActive(s == MenuScreen.Title);
        if (_levelRoot.IsValid) _levelRoot.SetActive(s == MenuScreen.LevelSelect);
        if (_settingsRoot.IsValid) _settingsRoot.SetActive(s == MenuScreen.Settings);
        if (_slotRoot.IsValid) _slotRoot.SetActive(s == MenuScreen.SlotSelect);
        IMenuScreen? scr = ActiveScreen();
        if (scr != null) { scr.OnShown(); _pendingShow = false; }
        else { _pendingShow = true; } // script attaches next frame; OnShown fires from OnUpdate then
    }

    // A full-screen custom-shader effect, drawn on top of every screen and the shell.
    private void CreateInk()
    {
        _inkFx = Ui.CreateEffect(Self, "ui_ink");
        Ui.SetEffectColors(_inkFx, InkColor, EdgeColor);
        // The flood must sit above every per-screen overlay (e.g. the title's ink drips) so it fully
        // swallows the outgoing screen instead of the drips bleeding through the transition.
        Ui.SetEffectSortOrder(_inkFx, 1000);
        SetInk(0f);
    }

    // coverage 0 = clear, 1 = fully covered. params = (coverage, time, noiseAmp, edgeWidth).
    private void SetInk(float coverage)
    {
        if (_inkFx.IsValid)
            Ui.SetEffectParams(_inkFx, new Vector4(coverage, _inkTime, NoiseAmp, EdgeWidth));
    }

    private static float EaseOut(float x) => 1f - (1f - x) * (1f - x);
    private static float EaseIn(float x) => x * x;

    // Resolved lazily from the registry each call, so it never races script attach order.
    private IMenuScreen? ActiveScreen() => Current switch
    {
        MenuScreen.Title => ScreenRegistry<TitleScreen>.Get("TitleRoot"),
        MenuScreen.LevelSelect => ScreenRegistry<LevelSelectScreen>.Get("LevelSelectRoot"),
        MenuScreen.Settings => ScreenRegistry<SettingsScreen>.Get("SettingsRoot"),
        MenuScreen.SlotSelect => ScreenRegistry<SlotSelectScreen>.Get("SlotSelectRoot"),
        _ => null,
    };
}
