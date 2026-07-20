using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Drives the GameHud prefab: live coin counter (level + run total) and the
/// level-complete banner. Attached to the prefab's canvas root, so every
/// level that spawns the prefab gets identical behaviour.
/// </summary>
public sealed class HudController : EntityScript
{
    private Entity _coinText;
    private Entity _banner;
    private int _shownCoins = -1;
    private bool _bannerShown;

    public override void OnAttach()
    {
        // The HUD outlives Scene.Load - one instance carries across levels
        // (children included), so there is no flicker or respawn per level.
        Self.DontDestroyOnLoad();
        _coinText = Scene.Find("HudCoinText");
        _banner = Scene.Find("HudBanner");
    }

    public override void OnUpdate(float deltaTime)
    {
        // A new level began (win state cleared): hide the banner again.
        if (_bannerShown && !GameState.Won)
        {
            _bannerShown = false;
            _shownCoins = -1;
            if (_banner.IsValid)
            {
                _banner.SetActive(false);
            }
        }

        if (_coinText.IsValid && GameState.Coins != _shownCoins)
        {
            _shownCoins = GameState.Coins;
            Ui.SetText(_coinText, GameState.TotalCoins > GameState.Coins
                ? $"x {GameState.Coins}   (run {GameState.TotalCoins})"
                : $"x {GameState.Coins}");
        }

        if (!_bannerShown && GameState.Won && _banner.IsValid)
        {
            _bannerShown = true;
            _banner.SetActive(true);
            Ui.SetText(_banner, GameState.NextSceneQueued ? "LEVEL COMPLETE!" : "YOU WIN!");
        }
    }
}
