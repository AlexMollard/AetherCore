using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Mario-style platformer movement: run, coyote-time jump with a press buffer,
/// variable jump height, sprite animation switching, facing flips, and a
/// fall-off-the-world respawn.
/// </summary>
public sealed class PlayerController : EntityScript
{
    public float MoveSpeed = 7.0f;
    public float JumpSpeed = 13.5f;
    public float CoyoteTime = 0.1f;
    public float JumpBuffer = 0.12f;
    public float FallRespawnY = -8.0f;

    private const string AnimIdle = "project://assets/animations/player_idle.spriteanim.toml";
    private const string AnimRun = "project://assets/animations/player_run.spriteanim.toml";
    private const string AnimJump = "project://assets/animations/player_jump.spriteanim.toml";

    /// <summary>Set on attach so other scripts (enemies, coins) can reach the player.</summary>
    public static PlayerController? Instance;

    private Vector3 _spawn;
    private float _sinceGrounded = 99.0f;
    private float _sinceJumpPressed = 99.0f;
    private string _anim = "";
    private float _facing = 1.0f;

    public override void OnAttach()
    {
        // Singleton guard (the Unity pattern for persistent actors): when a
        // DontDestroyOnLoad player crosses into a scene that authors its own
        // player, the scene's copy is the duplicate and removes itself. Every
        // level can safely author a player, so loading any level directly
        // still produces exactly one.
        if (Instance != null && Instance != this)
        {
            Log.Info("[CoinDash] Duplicate player removed (a persistent one already exists)");
            Self.Destroy();
            return;
        }
        Instance = this;
        _spawn = Self.Position;
        // Mid-run level transition keeps run totals; a fresh Play starts over.
        if (GameState.NextSceneQueued)
        {
            GameState.BeginLevel();
        }
        else
        {
            GameState.ResetRun();
        }
        Physics2D.EnableEvents(Self);
        SetAnim(AnimIdle);

        // Every level shares the same HUD prefab - one source of truth.
        if (!Scene.Find("GameHud").IsValid)
        {
            Scene.Instantiate("GameHud");
        }
    }

    public override void OnDetach()
    {
        if (Instance == this)
        {
            Instance = null;
        }
    }

    public override void OnUpdate(float deltaTime)
    {
        if (GameState.Won)
        {
            Physics2D.SetLinearVelocity(Self, new Vector2(0.0f, Physics2D.GetLinearVelocity(Self).Y));
            return;
        }

        float move = 0.0f;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.X = move * MoveSpeed;

        bool grounded = IsGrounded();
        _sinceGrounded = grounded ? 0.0f : _sinceGrounded + deltaTime;
        _sinceJumpPressed += deltaTime;
        if (Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up))
        {
            _sinceJumpPressed = 0.0f;
        }

        if (_sinceJumpPressed < JumpBuffer && _sinceGrounded < CoyoteTime)
        {
            velocity.Y = JumpSpeed;
            _sinceGrounded = 99.0f;
            _sinceJumpPressed = 99.0f;
        }
        // Variable jump height: releasing early clips the ascent.
        if (velocity.Y > 0.0f && !(Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up)))
        {
            velocity.Y *= 0.82f;
        }

        Physics2D.SetLinearVelocity(Self, velocity);

        if (move != 0.0f && System.MathF.Sign(move) != System.MathF.Sign(_facing))
        {
            _facing = move;
            Self.SetTransform(Self.Position, Self.EulerDegrees, new Vector3(_facing, 1.0f, 1.0f));
        }

        SetAnim(!grounded ? AnimJump : move != 0.0f ? AnimRun : AnimIdle);

        if (Self.Position.Y < FallRespawnY)
        {
            Respawn();
        }
    }

    public void Respawn()
    {
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
        Self.Position = _spawn;
    }

    /// <summary>Bounce used by enemies when the player stomps them.</summary>
    public void Bounce()
    {
        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.Y = JumpSpeed * 0.7f;
        Physics2D.SetLinearVelocity(Self, velocity);
    }

    private bool IsGrounded()
    {
        Vector3 pos = Self.Position;
        // Two rays near the capsule edges so ledges still count.
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
