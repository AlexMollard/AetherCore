using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

public enum MenuScreen { Title, LevelSelect, Settings }

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

    private Entity _titleRoot, _levelRoot, _settingsRoot;

    // Ink-flood transition (runtime overlay, created on attach; lives only during Play).
    private Entity _inkWipe, _inkEdge;
    private enum Phase { Idle, Cover, Reveal }
    private Phase _phase = Phase.Idle;
    private float _tt;
    private MenuScreen _pending;
    private const float CoverDur = 0.32f, RevealDur = 0.34f, InkMaxH = 1700f;
    private static readonly Vector4 InkColor = new(0.02f, 0.025f, 0.035f, 1f);
    private static readonly Vector4 EdgeColor = GameSettings.Accent;

    public override void OnAttach()
    {
        Instance = this;
        Time.Resume();
        GameSettings.Load();
        _titleRoot = Scene.Find("TitleRoot");
        _levelRoot = Scene.Find("LevelSelectRoot");
        _settingsRoot = Scene.Find("SettingsRoot");
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
        if (_phase == Phase.Idle)
        {
            if (Current != MenuScreen.Title && Input.IsKeyPressed(Key.Escape)) { Go(MenuScreen.Title); return; }
            ActiveScreen()?.HandleInput();
            return;
        }

        // Mid-transition: animate the ink and swallow input.
        _tt += dt;
        if (_phase == Phase.Cover)
        {
            float p = Math.Clamp(_tt / CoverDur, 0f, 1f);
            SetInk(InkMaxH * EaseOut(p)); // rise fast, settle at full cover
            if (p >= 1f) { ShowInstant(_pending); _phase = Phase.Reveal; _tt = 0f; }
        }
        else // Reveal
        {
            float p = Math.Clamp(_tt / RevealDur, 0f, 1f);
            SetInk(InkMaxH * (1f - EaseIn(p))); // hold, then drain away
            if (p >= 1f) { SetInk(0f); _phase = Phase.Idle; }
        }
    }

    private void ShowInstant(MenuScreen s)
    {
        Current = s;
        if (_titleRoot.IsValid) _titleRoot.SetActive(s == MenuScreen.Title);
        if (_levelRoot.IsValid) _levelRoot.SetActive(s == MenuScreen.LevelSelect);
        if (_settingsRoot.IsValid) _settingsRoot.SetActive(s == MenuScreen.Settings);
        ActiveScreen()?.OnShown();
    }

    // Full-width overlays anchored to the bottom edge; height animates upward via SetOffsets.
    // Created last, so they draw on top of every screen and the shell.
    private void CreateInk()
    {
        _inkWipe = Ui.CreateImage(Self);
        Ui.SetAnchors(_inkWipe, new Vector2(0f, 1f), new Vector2(1f, 1f));
        Ui.SetPivot(_inkWipe, new Vector2(0.5f, 1f));
        Ui.SetImageColor(_inkWipe, InkColor);

        _inkEdge = Ui.CreateImage(Self);
        Ui.SetAnchors(_inkEdge, new Vector2(0f, 1f), new Vector2(1f, 1f));
        Ui.SetPivot(_inkEdge, new Vector2(0.5f, 1f));
        Ui.SetImageColor(_inkEdge, EdgeColor);

        SetInk(0f);
    }

    private void SetInk(float h)
    {
        if (_inkWipe.IsValid)
            Ui.SetOffsets(_inkWipe, new Vector2(0f, -h), new Vector2(0f, 0f));
        if (_inkEdge.IsValid)
        {
            // A thin cyan "wet" line leading the ink's top edge; hidden once the ink is gone.
            Ui.SetOffsets(_inkEdge, new Vector2(0f, -h - 3f), new Vector2(0f, -h));
            Vector4 e = EdgeColor;
            e.W = h > 2f ? 0.9f : 0f;
            Ui.SetImageColor(_inkEdge, e);
        }
    }

    private static float EaseOut(float x) => 1f - (1f - x) * (1f - x);
    private static float EaseIn(float x) => x * x;

    // Resolved lazily from the registry each call, so it never races script attach order.
    private IMenuScreen? ActiveScreen() => Current switch
    {
        MenuScreen.Title => ScreenRegistry<TitleScreen>.Get("TitleRoot"),
        MenuScreen.LevelSelect => ScreenRegistry<LevelSelectScreen>.Get("LevelSelectRoot"),
        MenuScreen.Settings => ScreenRegistry<SettingsScreen>.Get("SettingsRoot"),
        _ => null,
    };
}
