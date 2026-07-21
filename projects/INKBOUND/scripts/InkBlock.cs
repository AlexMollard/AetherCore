using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A single conjured aether-ink platform block. Spawned by <see cref="AetherInk"/>
/// as the player draws.
///
/// The block only becomes a SOLID platform if, at the moment it is drawn, it is
/// "anchored" - within <see cref="AnchorRadius"/> of real geometry (the level, a
/// wall, or an aether crystal). Ink drawn in open air is unanchored: it turns to
/// harmless red "ghost" ink, non-solid, and crumbles almost immediately. That one
/// rule is what keeps free-drawing honest - you can't panic-draw a floor under a
/// mid-air fall, and you can't hop across the top of the level, because there is
/// nothing up there to anchor to. You extend ledges, bridge real gaps, and climb
/// near walls; the world decides what holds.
/// </summary>
public sealed class InkBlock : EntityScript
{
    public float Lifetime = 5.0f;
    public float FadeTime = 1.6f;
    /// <summary>How close to real geometry (or a crystal) the block must be to hold.</summary>
    public float AnchorRadius = 1.7f;

    // Live ink-block ids, so anchoring and "is the player on real ground?" checks
    // can tell conjured ink from actual terrain.
    private static readonly HashSet<uint> s_live = new();

    public static bool IsInk(uint id) => s_live.Contains(id);

    private float _age;
    private bool _anchored;
    private Vector4 _baseTint = new(0.45f, 0.75f, 1.0f, 1.0f);
    private Vector2 _baseSize;

    public override void OnAttach()
    {
        s_live.Add(Self.Id);
        _baseTint = SpriteRenderer.GetTint(Self);
        _baseSize = SpriteRenderer.GetPixelSize(Self);
        // Pop in with a tiny scale punch so drawing feels responsive.
        SpriteRenderer.SetPixelSize(Self, _baseSize * 0.6f);

        _anchored = EvaluateAnchor();
        if (!_anchored)
        {
            // Ghost ink: nothing to hold onto. Make it non-solid, mark it red, and
            // let it crumble fast so it reads as "that won't hold".
            Physics2D.SetTrigger(Self, true);
            Lifetime = 0.55f;
            FadeTime = 0.4f;
            _baseTint = new Vector4(1.0f, 0.34f, 0.3f, 0.7f);
            SpriteRenderer.SetTint(Self, _baseTint);
        }
        else
        {
            // The Ink Glow setting scales the conjured-ink glow (Settings screen, persisted).
            _baseTint.W *= 0.4f + 0.6f * GameSettings.InkGlow;
            SpriteRenderer.SetTint(Self, _baseTint);
        }
    }

    /// <summary>Is real geometry (or a crystal) within reach? Other ink does not
    /// count - a span must ultimately reach the actual world to hold.</summary>
    private bool EvaluateAnchor()
    {
        Vector3 p = Self.Position;
        uint playerId = PlayerController.Instance != null ? PlayerController.Instance.Self.Id : 0u;
        foreach (Entity e in Physics2D.OverlapCircle(new Vector2(p.X, p.Y), AnchorRadius))
        {
            uint id = e.Id;
            if (id == Self.Id || id == playerId || IsInk(id))
            {
                continue;
            }
            return true; // terrain or crystal within reach
        }
        return false;
    }

    public override void OnUpdate(float deltaTime)
    {
        _age += deltaTime;

        // Grow in over the first 0.1s.
        if (_age < 0.1f)
        {
            float g = 0.6f + 0.4f * (_age / 0.1f);
            SpriteRenderer.SetPixelSize(Self, _baseSize * g);
        }

        // Fade out over the tail of the lifetime, then vanish.
        if (_age > Lifetime - FadeTime)
        {
            float t = Math.Clamp((Lifetime - _age) / FadeTime, 0.0f, 1.0f);
            Vector4 tint = _baseTint;
            tint.W = _baseTint.W * t;
            SpriteRenderer.SetTint(Self, tint);
        }

        if (_age >= Lifetime)
        {
            Self.Destroy();
        }
    }

    public override void OnDetach()
    {
        s_live.Remove(Self.Id);
    }
}
