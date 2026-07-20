using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Bounce pad. When the player comes down onto it, it launches them straight up
/// much higher than a normal jump - enough to clear tall gaps or reach a coin
/// shelf. A quick squash-and-recover sells the boing, and a short cooldown stops
/// a single landing from firing twice.
/// </summary>
public sealed class SpringPad : EntityScript
{
    /// <summary>Upward launch velocity. The player's normal JumpSpeed is 16.5.</summary>
    public float LaunchSpeed = 27.0f;

    public float Cooldown = 0.15f;

    private float _cooldown;
    private float _squash;
    private Vector2 _baseSize;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        _baseSize = SpriteRenderer.GetPixelSize(Self);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id || _cooldown > 0.0f)
        {
            return;
        }
        // Trigger pad: touching it (running in or dropping on) launches the player.
        // A short cooldown stops the same overlap from firing twice a frame.
        player.Launch(LaunchSpeed);
        _cooldown = Cooldown;
        _squash = 0.55f;
        CameraFollow.Instance?.AddShake(0.04f);
        Log.Info("[CoinDash] Boing!");
    }

    public override void OnUpdate(float deltaTime)
    {
        if (_cooldown > 0.0f)
        {
            _cooldown -= deltaTime;
        }
        if (_squash > 0.0f)
        {
            _squash = Math.Max(0.0f, _squash - 4.5f * deltaTime);
            SpriteRenderer.SetPixelSize(Self, new Vector2(_baseSize.X * (1.0f + _squash * 0.35f), _baseSize.Y * (1.0f - _squash * 0.6f)));
        }
    }
}
