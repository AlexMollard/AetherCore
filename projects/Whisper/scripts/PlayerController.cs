using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Mario-style platformer movement: run, coyote-time jump with a press buffer,
/// variable jump height, drop-through one-way platforms, sprite animation
/// switching, facing flips, and a fall-off-the-world respawn.
/// </summary>
public sealed class PlayerController : EntityScript
{
    public float MoveSpeed = 7.0f;
    public float JumpSpeed = 16.5f;
    public float CoyoteTime = 0.12f;
    public float JumpBuffer = 0.15f;

    /// <summary>Falling below this Y snaps the player back to its spawn point.</summary>
    public float FallRespawnY = -8.0f;

    /// <summary>How long holding Down + jump keeps one-way platforms non-solid.</summary>
    public float DropThroughTime = 0.25f;

    /// <summary>Snappy platformer arc: ~0.33 s to apex instead of a floaty
    /// full-second hang (applied to the body's gravity scale on attach).</summary>
    public float GravityScale = 4.0f;

    /// <summary>Releasing jump while rising cuts the ascent to this fraction, once.</summary>
    public float JumpCutFactor = 0.45f;

    private const string AnimIdle = "project://assets/animations/player_idle.spriteanim.toml";
    private const string AnimRun = "project://assets/animations/player_run.spriteanim.toml";
    private const string AnimJump = "project://assets/animations/player_jump.spriteanim.toml";

    private Vector3 _spawn;
    private float _sinceGrounded = 99.0f;
    private float _sinceJumpPressed = 99.0f;
    private string _anim = "";
    private bool _jumpCutDone;

    // Squash & stretch is purely visual (drives the sprite quad size, never the
    // collider): stretch while airborne, squash impulse on landing, eased back.
    private Vector2 _baseSpriteSize = new(32.0f, 32.0f);
    private float _squash;       // +squashed (wide/short), -stretched (tall/thin)
    private bool _wasGrounded = true;

    public override void OnAttach()
    {
        _spawn = Self.Position;
        _baseSpriteSize = SpriteRenderer.GetPixelSize(Self);
        Physics2D.EnableEvents(Self);
        Physics2D.SetGravityScale(Self, GravityScale);
        SetAnim(AnimIdle);
    }

    public override void OnUpdate(float deltaTime)
    {
        // Frozen by a pause: scripts still tick at dt=0, so skip input/movement
        // entirely (otherwise a jump pressed while frozen would buffer).
        if (Time.IsPaused)
        {
            return;
        }

        float move = 0.0f;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.X = move * MoveSpeed;

        bool grounded = IsGrounded();
        // Landing squashes the sprite (visual only) - no screen shake here.
        if (grounded && !_wasGrounded)
        {
            _squash = 0.30f;
        }
        _wasGrounded = grounded;
        _sinceGrounded = grounded ? 0.0f : _sinceGrounded + deltaTime;
        _sinceJumpPressed += deltaTime;
        if (Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up))
        {
            _sinceJumpPressed = 0.0f;
        }

        if (grounded)
        {
            _jumpCutDone = false;
        }
        if (_sinceJumpPressed < JumpBuffer && _sinceGrounded < CoyoteTime)
        {
            bool holdingDown = Input.IsKeyDown(Key.S) || Input.IsKeyDown(Key.Down);
            if (holdingDown)
            {
                // Down + jump drops through a one-way platform instead of hopping. The
                // window only needs to outlast the fall past the platform's lip; if we
                // were standing on solid ground this simply does nothing and the jump
                // is spent, which is the same as every other platformer.
                Physics2D.SetDropThrough(Self, DropThroughTime);
                _sinceJumpPressed = 99.0f;
            }
            else
            {
                velocity.Y = JumpSpeed;
                _sinceGrounded = 99.0f;
                _sinceJumpPressed = 99.0f;
                _jumpCutDone = false;
                _squash = -0.24f; // launch stretch
            }
        }
        // Variable jump height: releasing early clips the ascent ONCE (a
        // per-frame multiplier would be framerate-dependent).
        if (!_jumpCutDone && velocity.Y > 0.0f && !(Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up)))
        {
            velocity.Y *= JumpCutFactor;
            _jumpCutDone = true;
        }

        Physics2D.SetLinearVelocity(Self, velocity);

        // Face the way we move via the sprite flag: negative transform scale
        // is clamped away by the 2D physics transform sync.
        if (move != 0.0f)
        {
            SpriteRenderer.SetFlipX(Self, move < 0.0f);
        }

        SetAnim(!grounded ? AnimJump : move != 0.0f ? AnimRun : AnimIdle);

        // Drive squash/stretch: motion-stretch while airborne, ease to neutral
        // on the ground, then push the sprite quad size (feet stay put enough
        // with the centre pivot at these small magnitudes).
        if (!grounded)
        {
            float wantStretch = -System.Math.Clamp(System.Math.Abs(velocity.Y) * 0.016f, 0.0f, 0.20f);
            _squash += (wantStretch - _squash) * System.Math.Clamp(10.0f * deltaTime, 0.0f, 1.0f);
        }
        else
        {
            _squash *= System.Math.Max(0.0f, 1.0f - 13.0f * deltaTime);
        }
        SpriteRenderer.SetPixelSize(Self, new Vector2(_baseSpriteSize.X * (1.0f + _squash), _baseSpriteSize.Y * (1.0f - _squash)));

        if (Self.Position.Y < FallRespawnY)
        {
            Respawn();
        }
    }

    /// <summary>Snap back to the entity's starting position after falling out of
    /// the world. There are no checkpoints in this build - it is always the spawn point.</summary>
    private void Respawn()
    {
        Self.Position = _spawn;
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
    }

    private bool IsGrounded()
    {
        Vector3 pos = Self.Position;
        // Two rays near the collider edges so ledges still count.
        for (float offset = -0.28f; offset <= 0.28f; offset += 0.56f)
        {
            RaycastHit2D hit = Physics2D.Raycast(new Vector2(pos.X + offset, pos.Y), new Vector2(0.0f, -1.0f), 0.85f);
            if (hit.DidHit)
            {
                return true;
            }
        }
        return false;
    }

    private void SetAnim(string path)
    {
        if (_anim == path)
        {
            return;
        }
        _anim = path;
        SpriteAnimator.SetAnimation(Self, path);
        SpriteAnimator.Play(Self);
    }
}
