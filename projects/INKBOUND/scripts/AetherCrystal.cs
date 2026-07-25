using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// An aether crystal: a floating pickup that instantly refills the ink meter
/// (see <see cref="AetherInk"/>). It turns the meter into a real resource - a
/// chasm too wide for one tank of aether becomes "grab the crystal mid-draw or
/// you run dry and fall". Collecting it hides the crystal, then it respawns after
/// a short delay so a failed attempt can be retried.
/// </summary>
public sealed class AetherCrystal : EntityScript
{
    public float RefillAmount = 45.0f;
    public float RespawnDelay = 6.0f;

    private float _phase;
    private bool _collected;
    private float _respawnTimer;
    private Vector4 _baseTint = new(0.45f, 0.85f, 1.0f, 1.0f);
    private Vector2 _baseSize;

    // Live crystal ids, so AetherInk can treat a crystal as a valid anchor without counting every
    // other trigger in the level (coins, dialogue zones, enemies) as one too.
    private static readonly System.Collections.Generic.HashSet<uint> s_crystals = new();

    public static bool IsCrystal(uint id) => s_crystals.Contains(id);

    public override void OnAttach()
    {
        Physics2D.SetTrigger(Self, true); // force the fixture to be a sensor at runtime
        Physics2D.EnableEvents(Self);
        _baseTint = SpriteRenderer.GetTint(Self);
        _baseSize = SpriteRenderer.GetPixelSize(Self);
        s_crystals.Add(Self.Id);
    }

    public override void OnDetach() => s_crystals.Remove(Self.Id);

    public override void OnUpdate(float deltaTime)
    {
        _phase += deltaTime;

        if (_collected)
        {
            _respawnTimer -= deltaTime;
            if (_respawnTimer <= 0.0f)
            {
                _collected = false;
                SpriteRenderer.SetTint(Self, _baseTint);
            }
            return;
        }

        // Shimmer + gentle pulse so it reads as live energy, no transform writes
        // (moving a physics body's transform fights the solver).
        float shimmer = 0.72f + 0.28f * (0.5f + 0.5f * (float)Math.Sin(_phase * 3.2f));
        SpriteRenderer.SetTint(Self, new Vector4(_baseTint.X, _baseTint.Y, _baseTint.Z, shimmer));
        float pulse = 1.0f + 0.07f * (float)Math.Sin(_phase * 3.2f);
        SpriteRenderer.SetPixelSize(Self, _baseSize * pulse);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (_collected || player == null || other.Id != player.Self.Id)
        {
            return;
        }

        AetherInk.Aether = Math.Min(AetherInk.AetherMax, AetherInk.Aether + RefillAmount);
        _collected = true;
        _respawnTimer = RespawnDelay;
        SpriteRenderer.SetTint(Self, new Vector4(_baseTint.X, _baseTint.Y, _baseTint.Z, 0.0f)); // hide until respawn
        Log.Info($"[INKBOUND] Aether crystal collected (+{RefillAmount})");
    }
}
