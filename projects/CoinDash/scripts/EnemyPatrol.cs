using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Goomba-style patroller: walks until it meets a wall or a ledge, then turns.
/// Stomping it from above squashes it (bouncing the player); touching it from
/// the side sends the player back to spawn.
/// </summary>
public sealed class EnemyPatrol : EntityScript
{
    public float WalkSpeed = 2.0f;
    public float SquashSeconds = 0.45f;

    private const string AnimWalk = "project://assets/animations/enemy_walk.spriteanim.toml";
    private const string AnimSquash = "project://assets/animations/enemy_squash.spriteanim.toml";

    private float _direction = -1.0f;
    private float _squashTimer = -1.0f;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        SpriteAnimator.SetAnimation(Self, AnimWalk);
        SpriteAnimator.Play(Self);
    }

    public override void OnUpdate(float deltaTime)
    {
        if (_squashTimer >= 0.0f)
        {
            _squashTimer -= deltaTime;
            if (_squashTimer <= 0.0f)
            {
                Self.Destroy();
            }
            return;
        }

        Vector3 pos = Self.Position;
        // Turn at walls and at ledges (no ground one step ahead).
        Vector2 ahead = new(pos.X + _direction * 0.55f, pos.Y);
        RaycastHit2D wallHit = Physics2D.Raycast(new Vector2(pos.X, pos.Y), new Vector2(_direction, 0.0f), 0.6f);
        bool wall = wallHit.DidHit;
        bool ledge = !Physics2D.Raycast(ahead, new Vector2(0.0f, -1.0f), 0.9f).DidHit;
        if (wall || ledge)
        {
            Log.Info($"[EnemyDbg] #{Self.Id} turn x={pos.X:F3} dir={_direction} wall={wall} hit=#{wallHit.Entity.Id} f={wallHit.Fraction:F3} ledge={ledge}");
            _direction = -_direction;
        }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.X = _direction * WalkSpeed;
        Physics2D.SetLinearVelocity(Self, velocity);
    }

    public override void OnCollisionEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id || _squashTimer >= 0.0f)
        {
            return;
        }
        // Above and falling = stomp; anything else hurts.
        bool stomp = other.Position.Y > Self.Position.Y + 0.35f && Physics2D.GetLinearVelocity(other).Y <= 0.5f;
        if (stomp)
        {
            _squashTimer = SquashSeconds;
            Physics2D.SetLinearVelocity(Self, Vector2.Zero);
            SpriteAnimator.SetAnimation(Self, AnimSquash);
            SpriteAnimator.SetLoopMode(Self, SpriteAnimationLoopMode.Hold);
            SpriteAnimator.Play(Self);
            player.Bounce();
            Log.Info("[CoinDash] Enemy stomped!");
        }
        else
        {
            Log.Info("[CoinDash] Ouch! Back to the start.");
            player.Respawn();
        }
    }
}
