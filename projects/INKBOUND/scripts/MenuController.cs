using System.Collections.Generic;
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

/// <summary>Menu state machine: shows exactly one screen root, routes per-frame input to
/// that screen's controller, and handles global Escape (back to Title).</summary>
public sealed class MenuController : EntityScript
{
    public static MenuController? Instance;
    public MenuScreen Current { get; private set; } = MenuScreen.Title;

    private Entity _titleRoot, _levelRoot, _settingsRoot;

    public override void OnAttach()
    {
        Instance = this;
        Time.Resume();
        GameSettings.Load();
        _titleRoot = Scene.Find("TitleRoot");
        _levelRoot = Scene.Find("LevelSelectRoot");
        _settingsRoot = Scene.Find("SettingsRoot");
        Go(MenuScreen.Title);
    }

    public void Go(MenuScreen s)
    {
        Current = s;
        if (_titleRoot.IsValid) _titleRoot.SetActive(s == MenuScreen.Title);
        if (_levelRoot.IsValid) _levelRoot.SetActive(s == MenuScreen.LevelSelect);
        if (_settingsRoot.IsValid) _settingsRoot.SetActive(s == MenuScreen.Settings);
        ActiveScreen()?.OnShown();
    }

    public override void OnUpdate(float dt)
    {
        if (Current != MenuScreen.Title && Input.IsKeyPressed(Key.Escape)) { Go(MenuScreen.Title); return; }
        ActiveScreen()?.HandleInput();
    }

    // Resolved lazily from the registry each call, so it never races script attach order.
    private IMenuScreen? ActiveScreen() => Current switch
    {
        MenuScreen.Title => ScreenRegistry<TitleScreen>.Get("TitleRoot"),
        MenuScreen.LevelSelect => ScreenRegistry<LevelSelectScreen>.Get("LevelSelectRoot"),
        MenuScreen.Settings => ScreenRegistry<SettingsScreen>.Get("SettingsRoot"),
        _ => null,
    };
}
