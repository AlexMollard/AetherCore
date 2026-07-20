using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Minimal top-down controller for the Physics2D collision sandbox: WASD moves
/// a zero-gravity body through velocity so walls and triggers behave.
/// </summary>
public sealed class TopDownController2D : EntityScript
{
    public float MoveSpeed = 5.0f;

    public override void OnAttach()
        => Physics2D.SetGravityScale(Self, 0.0f);

    public override void OnUpdate(float deltaTime)
    {
        Vector2 move = Vector2.Zero;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move.X -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move.X += 1.0f; }
        if (Input.IsKeyDown(Key.S) || Input.IsKeyDown(Key.Down)) { move.Y -= 1.0f; }
        if (Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up)) { move.Y += 1.0f; }
        if (move != Vector2.Zero)
        {
            move = Vector2.Normalize(move);
        }
        Physics2D.SetLinearVelocity(Self, move * MoveSpeed);
    }

    public override void OnTriggerEnter2D(Entity other)
        => Log.Info($"[TopDown] entered zone #{other.Id}");

    public override void OnTriggerExit2D(Entity other)
        => Log.Info($"[TopDown] left zone #{other.Id}");
}
