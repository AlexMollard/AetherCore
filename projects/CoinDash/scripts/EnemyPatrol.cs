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
        bool wall = Physics2D.Raycast(new Vector2(pos.X, pos.Y), new Vector2(_direction, 0.0f), 0.6f).DidHit;
        bool ledge = !Physics2D.Raycast(ahead, new Vector2(0.0f, -1.0f), 0.9f).DidHit;
        if (wall || ledge)
        {
            _direction = -_direction;
        }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.X = _direction * WalkSpeed;
        Physics2D.SetLinearVelocity(Self, velocity);
        SpriteRenderer.SetFlipX(Self, _direction > 0.0f);
    }

    public override void OnCollisionEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id || _squashTimer >= 0.0f)
        {
            return;
        }
        // Stomp when the player's feet are above the slime's midline; the
        // capsule's half extent is ~0.65, the slime box's half height 0.35.
        float feetY = other.Position.Y - 0.6f;
        bool stomp = feetY > Self.Position.Y;
        if (stomp)
        {
            _squashTimer = SquashSeconds;
            // Death: stop moving, and drop solid collision so the player can't
            // stand on the corpse mid-squash. Sensor keeps the body from
            // sinking oddly - zeroed gravity + velocity pin it in place while
            // the squash animation plays out, then OnUpdate destroys it.
            Physics2D.SetLinearVelocity(Self, Vector2.Zero);
            Physics2D.SetGravityScale(Self, 0.0f);
            Physics2D.SetTrigger(Self, true);
            SpriteAnimator.SetAnimation(Self, AnimSquash);
            SpriteAnimator.SetLoopMode(Self, SpriteAnimationLoopMode.Hold);
            SpriteAnimator.Play(Self);
            player.Bounce();
            CameraFollow.Instance?.AddShake(0.07f); // subtle kill feedback
            Log.Info("[CoinDash] Enemy stomped!");
        }
        else
        {
            Log.Info("[CoinDash] Ouch! The slime got you.");
            player.Die();
        }
    }
}
