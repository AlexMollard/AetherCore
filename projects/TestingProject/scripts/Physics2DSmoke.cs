using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Exercises the managed Physics2D surface end to end: 2D collision callbacks,
/// a downward raycast, an overlap query, and an impulse. Attach to a dynamic
/// 2D body above a static ground and watch the console.
/// </summary>
public sealed class Physics2DSmoke : EntityScript
{
    private bool _bounced;
    private float _logTimer;

    public override void OnAttach()
    {
        // Make the 2D collider wireframes visible for the smoke run.
        Debug.DrawEnabled = true;
        Physics.DebugDrawEnabled = true;
    }

    public override void OnCollisionEnter2D(Entity other)
    {
        Log.Info($"[P2D] collision enter with #{other.Id} at velocity {Physics2D.GetLinearVelocity(Self)}");
        if (!_bounced)
        {
            _bounced = true;
            Physics2D.AddImpulse(Self, new Vector2(0.0f, 4.0f));
            Log.Info("[P2D] bounce impulse applied");
        }
    }

    public override void OnCollisionExit2D(Entity other)
        => Log.Info($"[P2D] collision exit with #{other.Id}");

    public override void OnUpdate(float deltaTime)
    {
        _logTimer += deltaTime;
        if (_logTimer < 1.0f)
        {
            return;
        }
        _logTimer = 0.0f;

        Vector3 pos = Self.Position;
        RaycastHit2D hit = Physics2D.Raycast(new Vector2(pos.X, pos.Y), new Vector2(0.0f, -1.0f), 50.0f);
        if (hit.DidHit)
        {
            Log.Info($"[P2D] ground below at y={hit.Point.Y:F2} (entity #{hit.Entity.Id}, fraction {hit.Fraction:F2})");
        }

        Entity[] near = Physics2D.OverlapCircle(new Vector2(pos.X, pos.Y), 2.0f);
        Log.Info($"[P2D] {near.Length} bodies within 2 units");
    }
}
