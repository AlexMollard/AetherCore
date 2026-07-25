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
    private Entity _clock;
    private int _shownCoins = -1;
    private bool _bannerShown;

    // Aether meter for the ink mechanic (created at runtime so no prefab edit
    // is needed; see AetherInk). Sits just above the coin counter, bottom-left.
    private Entity _aetherFill;
    private float _shownAether = -1.0f;
    // The meter is an ink WELL, not a bar: a squat pot in the corner whose level drops as you draw.
    // Reading your remaining ink off the amount of ink in a pot needs no legend.
    private const float WellX = 28.0f;
    private const float WellY = 74.0f;
    private const float WellW = 54.0f;
    private const float WellH = 66.0f;

    public override void OnAttach()
    {
        // The HUD outlives Scene.Load - one instance carries across levels
        // (children included), so there is no flicker or respawn per level.
        Self.DontDestroyOnLoad();
        _coinText = Scene.Find("HudCoinText");
        _banner = Scene.Find("HudBanner");
        _clock = Scene.Find("HudTrialClock");
    }

    /// <summary>Create the aether bar if it does not exist yet (also re-runs
    /// after a scene load should the runtime children not carry across).</summary>
    private void EnsureBar()
    {
        if (_aetherFill.IsValid)
        {
            return;
        }

        // One widget does the whole thing: the ui_inkwell material draws the pot, the liquid and its
        // meniscus, driven by a single fill parameter. No separate background and fill rect to keep
        // in sync, and the level can slosh.
        _aetherFill = Ui.CreateImage(Self);
        Ui.SetAnchors(_aetherFill, new Vector2(0.0f, 0.0f), new Vector2(0.0f, 0.0f));
        Ui.SetPivot(_aetherFill, new Vector2(0.0f, 0.0f));
        Ui.SetRect(_aetherFill, WellX, WellY, WellW, WellH);
        Ui.SetImageColor(_aetherFill, Vector4.One);
        Ui.SetImageCornerRadius(_aetherFill, 6.0f);
        Ui.SetMaterial(_aetherFill, "ui_inkwell");
        _shownAether = -1.0f;
    }

    public override void OnUpdate(float deltaTime)
    {
        EnsureBar();

        float max = AetherInk.AetherMax > 0.0f ? AetherInk.AetherMax : 1.0f;
        float frac = System.Math.Clamp(AetherInk.Aether / max, 0.0f, 1.0f);
        if (_aetherFill.IsValid)
        {
            // Ease the shown level toward the real one so spending ink pours out rather than snapping,
            // and feed time as well so the surface keeps sloshing even while the value is steady.
            _shownAether = _shownAether < 0.0f ? frac : _shownAether + (frac - _shownAether)
                                                      * System.Math.Clamp(9.0f * deltaTime, 0.0f, 1.0f);
            Ui.SetMaterialParams(_aetherFill, new Vector4(Time.UnscaledTime, _shownAether, 0.0f, 0.0f));
        }

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

        if (_clock.IsValid)
        {
            if (GameState.TrialMode)
            {
                int total = (int)GameState.TrialElapsed;
                Ui.SetText(_clock, $"{total / 60:00}:{total % 60:00}.{(int)((GameState.TrialElapsed - total) * 100):00}");
            }
            else Ui.SetText(_clock, "");
        }
    }
}
