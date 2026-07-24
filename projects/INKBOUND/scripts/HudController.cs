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
    private Entity _aetherBg;
    private Entity _aetherFill;
    private float _shownAether = -1.0f;
    private const float BarX = 28.0f;
    private const float BarY = 86.0f;
    private const float BarW = 220.0f;
    private const float BarH = 18.0f;

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

        _aetherBg = Ui.CreateImage(Self);
        Ui.SetAnchors(_aetherBg, new Vector2(0.0f, 0.0f), new Vector2(0.0f, 0.0f));
        Ui.SetPivot(_aetherBg, new Vector2(0.0f, 0.0f));
        Ui.SetRect(_aetherBg, BarX, BarY, BarW, BarH);
        Ui.SetImageColor(_aetherBg, new Vector4(0.04f, 0.06f, 0.10f, 0.72f));
        Ui.SetImageCornerRadius(_aetherBg, 4.0f);

        _aetherFill = Ui.CreateImage(Self);
        Ui.SetAnchors(_aetherFill, new Vector2(0.0f, 0.0f), new Vector2(0.0f, 0.0f));
        Ui.SetPivot(_aetherFill, new Vector2(0.0f, 0.0f));
        Ui.SetRect(_aetherFill, BarX + 2.0f, BarY + 2.0f, BarW - 4.0f, BarH - 4.0f);
        Ui.SetImageCornerRadius(_aetherFill, 3.0f);
        _shownAether = -1.0f;
    }

    public override void OnUpdate(float deltaTime)
    {
        EnsureBar();

        float max = AetherInk.AetherMax > 0.0f ? AetherInk.AetherMax : 1.0f;
        float frac = System.Math.Clamp(AetherInk.Aether / max, 0.0f, 1.0f);
        if (_aetherFill.IsValid && System.Math.Abs(frac - _shownAether) > 0.001f)
        {
            _shownAether = frac;
            Ui.SetRect(_aetherFill, BarX + 2.0f, BarY + 2.0f, (BarW - 4.0f) * frac, BarH - 4.0f);
            // Aether-cyan normally; warm amber when nearly spent so the player
            // knows the ink is about to cut out.
            Ui.SetImageColor(_aetherFill, frac < 0.25f
                ? new Vector4(1.0f, 0.62f, 0.24f, 0.95f)
                : new Vector4(0.34f, 0.72f, 1.0f, 0.95f));
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
