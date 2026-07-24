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
    public float JumpSpeed = 16.5f;
    public float CoyoteTime = 0.12f;
    public float JumpBuffer = 0.15f;
    public float FallRespawnY = -8.0f;

    /// <summary>How long holding Down + jump keeps one-way platforms non-solid.</summary>
    public float DropThroughTime = 0.25f;

    /// <summary>Snappy platformer arc: ~0.33 s to apex instead of a floaty
    /// full-second hang (applied to the body's gravity scale on attach).</summary>
    public float GravityScale = 4.0f;

    /// <summary>Releasing jump while rising cuts the ascent to this fraction, once.</summary>
    public float JumpCutFactor = 0.45f;

    /// <summary>Death feel: the corpse pops up with this speed and falls through the
    /// world, and the whole death beat lasts <see cref="DeathDuration"/> before the
    /// respawn at the last checkpoint.</summary>
    public float DeathHopSpeed = 13.0f;
    public float DeathDuration = 0.7f;

    private const string AnimIdle = "project://assets/animations/player_idle.spriteanim.toml";
    private const string AnimRun = "project://assets/animations/player_run.spriteanim.toml";
    private const string AnimJump = "project://assets/animations/player_jump.spriteanim.toml";

    /// <summary>Set on attach so other scripts (enemies, coins) can reach the player.</summary>
    public static PlayerController? Instance;

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
    private float _prevVelY;

    // Death sequence state (see Die/UpdateDeath): while dead the player ignores
    // input and world collision and plays out the death beat.
    private bool _dead;
    private float _deathTimer;

    public override void OnAttach()
    {
        // Singleton guard (the Unity pattern for persistent actors): when a
        // DontDestroyOnLoad player crosses into a scene that authors its own
        // player, the scene's copy is the duplicate and removes itself. Every
        // level can safely author a player, so loading any level directly
        // still produces exactly one.
        if (Instance != null && Instance != this)
        {
            Log.Info("[INKBOUND] Duplicate player removed (a persistent one already exists)");
            Self.Destroy();
            return;
        }
        Instance = this;
        _spawn = Self.Position;
        _baseSpriteSize = SpriteRenderer.GetPixelSize(Self);
        // Mid-run level transition keeps run totals; a fresh Play starts over.
        if (GameState.NextSceneQueued)
        {
            GameState.BeginLevel();
            // A time trial only ever covers the single level it was armed for (Win()
            // already recorded that level's best time before this scene load); clear it
            // here so the auto-advance chain doesn't keep running the trial clock and
            // recording best-times for every level after it.
            GameState.TrialMode = false;
        }
        else
        {
            GameState.ResetRun();
        }
        Physics2D.EnableEvents(Self);
        Physics2D.SetGravityScale(Self, GravityScale);
        SetAnim(AnimIdle);

        // Every level shares the same HUD prefab - one source of truth.
        if (!Scene.Find("GameHud").IsValid)
        {
            Scene.Instantiate("GameHud");
        }

        // Same one-instance pattern for the dialogue presenter (DontDestroyOnLoad; carried across levels).
        if (!Scene.Find("DialogueRunner").IsValid)
        {
            Scene.Instantiate("DialogueRunner");
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
        // Frozen by the pause menu or an open dialogue: scripts still tick at dt=0, so skip
        // input/movement entirely (otherwise a jump/draw pressed while frozen would buffer).
        if (Time.IsPaused || Dialogue.IsActive)
        {
            return;
        }

        if (GameState.TrialMode && !GameState.Won) GameState.TrialElapsed += deltaTime;

        if (GameState.Won)
        {
            Physics2D.SetLinearVelocity(Self, new Vector2(0.0f, Physics2D.GetLinearVelocity(Self).Y));
            return;
        }

        // Death owns the player until the respawn beat is up - no input, no movement.
        if (_dead)
        {
            UpdateDeath(deltaTime);
            return;
        }

        float move = 0.0f;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }

        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        float readVelY = velocity.Y; // pre-jump read, used for landing impact
        velocity.X = move * MoveSpeed;

        bool grounded = IsGrounded();
        // Landing squashes the sprite (visual only) - no screen shake here;
        // shake is reserved for kills and deaths so it stays meaningful.
        if (grounded && !_wasGrounded)
        {
            _squash = 0.30f;
            // Kick up a dust puff at the feet on a real fall (not tiny hops).
            if (-_prevVelY > 3.0f)
            {
                Scene.Instantiate("Dust", new Vector3(Self.Position.X, Self.Position.Y - 0.6f, 0.0f));
            }
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
        _prevVelY = readVelY;

        if (Self.Position.Y < FallRespawnY)
        {
            Die();
        }
    }

    /// <summary>Move the respawn point (checkpoints). The player returns here on a
    /// hazard hit or a fall instead of the level start.</summary>
    public void SetCheckpoint(Vector3 position)
    {
        _spawn = position;
    }

    /// <summary>Hard-move the player (used by checkpoint resume) and reset the checkpoint anchor there.</summary>
    public void TeleportTo(Vector3 position)
    {
        _spawn = position;
        Self.Position = position;
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
    }

    /// <summary>Kill the player. Instead of snapping straight back, the corpse pops
    /// up and arcs down THROUGH the world (Mario-style) with a hurt flash and a
    /// screen shake, then <see cref="RespawnNow"/> restores control at the last
    /// checkpoint after <see cref="DeathDuration"/>. Called by hazards, enemies and
    /// the fall-off-the-world check; re-entrant calls are ignored while already dead.</summary>
    public void Die()
    {
        if (_dead)
        {
            return;
        }
        _dead = true;
        _deathTimer = 0.0f;
        // Sensor so the body ignores the ground and floats off; gravity still pulls
        // it back down for the arc.
        Physics2D.SetTrigger(Self, true);
        Physics2D.SetGravityScale(Self, GravityScale);
        Physics2D.SetLinearVelocity(Self, new Vector2(0.0f, DeathHopSpeed));
        SetAnim(AnimJump);
        SpriteRenderer.SetTint(Self, new Vector4(1.0f, 0.4f, 0.35f, 1.0f)); // hurt flash
        CameraFollow.Instance?.AddShake(0.22f);
        // Layered burst: spark explosion + debris + smoke (all in the one prefab),
        // plus a ground puff at the feet.
        Scene.Instantiate("DeathBurst", new Vector3(Self.Position.X, Self.Position.Y, 0.0f));
        Scene.Instantiate("Dust", new Vector3(Self.Position.X, Self.Position.Y - 0.5f, 0.0f));
        Log.Info("[INKBOUND] Player down - respawning at the checkpoint.");
    }

    /// <summary>Per-frame death beat: the corpse shrinks and fades as it falls, then
    /// respawns when the timer runs out.</summary>
    private void UpdateDeath(float deltaTime)
    {
        _deathTimer += deltaTime;
        float k = System.Math.Clamp(_deathTimer / DeathDuration, 0.0f, 1.0f);
        float scale = 1.0f - 0.55f * k;
        SpriteRenderer.SetPixelSize(Self, new Vector2(_baseSpriteSize.X * scale, _baseSpriteSize.Y * scale));
        SpriteRenderer.SetTint(Self, new Vector4(1.0f, 0.4f, 0.35f, 1.0f - k));
        if (_deathTimer >= DeathDuration)
        {
            RespawnNow();
        }
    }

    /// <summary>Tail of the death sequence: snap back to the last checkpoint, restore
    /// solid collision and control, and pop a spawn puff.</summary>
    private void RespawnNow()
    {
        _dead = false;
        _squash = 0.0f;
        Self.Position = _spawn;
        Physics2D.SetTrigger(Self, false);
        Physics2D.SetGravityScale(Self, GravityScale);
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
        SpriteRenderer.SetPixelSize(Self, _baseSpriteSize);
        SpriteRenderer.SetTint(Self, Vector4.One);
        SetAnim(AnimIdle);
        CameraFollow.Instance?.AddShake(0.06f);
        // Materialise sparkle at the checkpoint.
        Scene.Instantiate("RespawnPop", new Vector3(_spawn.X, _spawn.Y, 0.0f));
    }

    /// <summary>Bounce used by enemies when the player stomps them.</summary>
    public void Bounce()
    {
        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.Y = JumpSpeed * 0.7f;
        Physics2D.SetLinearVelocity(Self, velocity);
    }

    /// <summary>Set an explicit upward launch (spring pads). Cancels the variable-jump
    /// cut for this ascent so the full launch height always lands, whether or not the
    /// player is holding jump, and adds a launch stretch.</summary>
    public void Launch(float speed)
    {
        Vector2 velocity = Physics2D.GetLinearVelocity(Self);
        velocity.Y = speed;
        Physics2D.SetLinearVelocity(Self, velocity);
        _jumpCutDone = true;
        _squash = -0.3f;
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
