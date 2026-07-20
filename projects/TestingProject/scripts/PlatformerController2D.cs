using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Minimal 2D platformer controller for the Physics2D reference room:
/// A/D or arrows to run, Space to jump when a downward ray finds ground.
/// Drives the Box2D body through velocity, never the transform.
/// </summary>
public sealed class PlatformerController2D : EntityScript
{
    public float MoveSpeed = 6.0f;
    public float JumpSpeed = 9.0f;

    /// <summary>Half the body height plus a small skin, for the grounded ray.</summary>
    public float GroundProbe = 0.6f;

    public override void OnUpdate(float deltaTime)
    {
        float move = 0.0f;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.X = move * MoveSpeed;

        Vector3 pos = Self.Position;
        RaycastHit2D ground = Physics2D.Raycast(new Vector2(pos.X, pos.Y), new Vector2(0.0f, -1.0f), GroundProbe);
        if (ground.DidHit && (Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up)))
        {
            velocity.Y = JumpSpeed;
        }

        Physics2D.SetLinearVelocity(Self, velocity);
    }

    public override void OnTriggerEnter2D(Entity other)
        => Log.Info($"[Platformer] entered trigger #{other.Id}");
}
