using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A crawling thing: walks until it meets a wall or a ledge, then turns. Touching it kills you,
/// from any direction - there is no jumping on heads here. You unmake it the way you do everything
/// else, by drawing ink across it (see <see cref="Creature"/> and <see cref="AetherInk"/>).
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
        Creature.Register(Self.Id, Smother);
    }

    public override void OnDetach() => Creature.Unregister(Self.Id);

    /// <summary>Drowned in ink: it stops, goes limp and is unmade. Called by the ink, not by a stomp.</summary>
    private void Smother()
    {
        if (_squashTimer >= 0.0f) { return; }
        _squashTimer = SquashSeconds;
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
        Physics2D.SetGravityScale(Self, 0.0f);
        Physics2D.SetTrigger(Self, true);
        SpriteAnimator.SetAnimation(Self, AnimSquash);
        SpriteAnimator.SetLoopMode(Self, SpriteAnimationLoopMode.Hold);
        SpriteAnimator.Play(Self);
        SpriteRenderer.SetTint(Self, new System.Numerics.Vector4(0.25f, 0.35f, 0.45f, 1.0f));
        Scene.Instantiate("DeathInk", Self.Position);
        CameraFollow.Instance?.AddShake(0.06f);
        Log.Info("[INKBOUND] Something drowned in the ink.");
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
        // No stomping. Touching one of these is fatal from every angle - the answer is to draw over
        // it, not to land on it.
        Log.Info("[INKBOUND] It found you in the dark.");
        player.Die();
    }
}
