using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Spinning pickup: touching the trigger collects it, then it pops -
/// a quick scale-punch and fade - before removing itself.</summary>
public sealed class Coin : EntityScript
{
    private const float PopSeconds = 0.16f;

    private bool _collected;
    private float _popTimer;
    private Vector2 _baseSize;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        _baseSize = SpriteRenderer.GetPixelSize(Self);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        if (_collected)
        {
            return;
        }
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id)
        {
            return;
        }
        // Count it immediately (HUD updates now); the pop is purely visual.
        // The _collected guard blocks re-entry - we must NOT make the coin solid
        // (that would bump the player), so the trigger is left as-is.
        GameState.CollectCoin();
        _collected = true;
        _popTimer = 0.0f;
        // Golden sparkle burst - a self-destructing effect entity, so it
        // outlives this coin's own removal.
        Scene.Instantiate("CoinSparkle", Self.Position);
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_collected)
        {
            return;
        }
        _popTimer += deltaTime;
        float t = Math.Clamp(_popTimer / PopSeconds, 0.0f, 1.0f);
        // Punch up to 1.6x while fading out, and drift up a touch.
        float scale = 1.0f + 0.6f * t;
        SpriteRenderer.SetPixelSize(Self, _baseSize * scale);
        Vector4 tint = SpriteRenderer.GetTint(Self);
        tint.W = 1.0f - t;
        SpriteRenderer.SetTint(Self, tint);
        Vector3 pos = Self.Position;
        pos.Y += 2.5f * deltaTime;
        Self.Position = pos;

        if (t >= 1.0f)
        {
            Self.Destroy();
        }
    }
}
