using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A heavy rock the world can move. It is the counterweight the ink cannot be: drawn ink expires,
/// so anything that needs holding down permanently needs one of these instead.
///
/// It is an ordinary dynamic body, which is the whole point - it rolls down whatever slope you draw,
/// stops against whatever wall you draw, and rests where you leave it. It also puts itself back if it
/// ends up somewhere unrecoverable, so a puzzle can never be permanently lost.
/// </summary>
public sealed class Boulder : EntityScript
{
    /// <summary>Falling below this returns the boulder to where it started.</summary>
    public float FallResetY = -8.0f;
    /// <summary>Seconds of stillness far from home before it resets anyway (wedged somewhere useless).</summary>
    public float StuckResetSeconds = 0.0f; // 0 disables

    private static readonly HashSet<uint> s_boulders = new();

    /// <summary>Is this entity a boulder? Used by plates so ink and stray props cannot weigh them.</summary>
    public static bool IsBoulder(uint id) => s_boulders.Contains(id);

    private Vector3 _home;
    private float _stillFor;

    public override void OnAttach()
    {
        s_boulders.Add(Self.Id);
        Physics2D.EnableEvents(Self);
        _home = Self.Position;
    }

    public override void OnDetach() => s_boulders.Remove(Self.Id);

    public override void OnUpdate(float deltaTime)
    {
        Vector3 p = Self.Position;
        if (p.Y < FallResetY)
        {
            ResetHome();
            return;
        }

        if (StuckResetSeconds > 0.0f)
        {
            Vector2 v = Physics2D.GetLinearVelocity(Self);
            bool settled = v.LengthSquared() < 0.02f;
            bool farFromHome = Vector2.Distance(new Vector2(p.X, p.Y), new Vector2(_home.X, _home.Y)) > 1.5f;
            _stillFor = settled && farFromHome ? _stillFor + deltaTime : 0.0f;
        }
    }

    private void ResetHome()
    {
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
        Self.Position = _home;
        Scene.Instantiate("Dust", _home);
    }
}
