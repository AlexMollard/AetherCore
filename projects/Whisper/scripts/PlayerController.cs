using System.Collections.Generic;
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

    // Internal (not private) so NetPlayerSync - the sole place that actually calls
    // SpriteAnimator, see that script's header comment - can resolve an AnimIndex
    // into a clip path.
    internal const string AnimIdle = "project://assets/animations/player_idle.spriteanim.toml";
    internal const string AnimRun = "project://assets/animations/player_run.spriteanim.toml";
    internal const string AnimJump = "project://assets/animations/player_jump.spriteanim.toml";

    /// <summary>Wire/AnimIndex values for the three clips above.</summary>
    internal const int AnimIndexIdle = 0;
    internal const int AnimIndexRun = 1;
    internal const int AnimIndexJump = 2;

    // NetPlayerSync needs to read this entity's PlayerController, but the engine has
    // no generic "get sibling script on this entity" API. Rather than add one at the
    // engine layer for a single call site, PlayerController tracks its own live
    // instances by entity id - confined to this game assembly, cleaned up in
    // OnDetach below.
    private static readonly Dictionary<uint, PlayerController> s_byEntity = new();

    /// <summary>The PlayerController instance attached to <paramref name="entity"/>,
    /// or null if none is live there.</summary>
    internal static PlayerController? For(Entity entity)
        => s_byEntity.TryGetValue(entity.Id, out PlayerController? controller) ? controller : null;

    /// <summary>
    /// The clip that movement/physics state currently calls for - one of the
    /// AnimIndex* constants above. Computed here every frame (this script already
    /// owns the grounded/movement state that decides it) but NOT applied to the
    /// sprite here; NetPlayerSync reads it, replicates it, and is the one place
    /// that calls SpriteAnimator, so the owner and every remote copy switch clips
    /// through the identical path instead of two scripts guessing independently.
    /// </summary>
    public int AnimIndex { get; private set; }

    private Vector3 _spawn;
    private float _sinceGrounded = 99.0f;
    private float _sinceJumpPressed = 99.0f;
    private bool _jumpCutDone;
    private bool _nameReported;

    // Squash & stretch is purely visual (drives the sprite quad size, never the
    // collider): stretch while airborne, squash impulse on landing, eased back.
    private Vector2 _baseSpriteSize = new(32.0f, 32.0f);
    private float _squash;       // +squashed (wide/short), -stretched (tall/thin)
    private bool _wasGrounded = true;

    public override void OnAttach()
    {
        s_byEntity[Self.Id] = this;
        _spawn = Self.Position;
        _baseSpriteSize = SpriteRenderer.GetPixelSize(Self);
        Physics2D.EnableEvents(Self);
        Physics2D.SetGravityScale(Self, GravityScale);
    }

    public override void OnDetach()
    {
        s_byEntity.Remove(Self.Id);
    }

    public override void OnUpdate(float deltaTime)
    {
        // Every client runs this script on every player entity, including other
        // people's. Only the owner reads input; the rest are driven by replication.
        // Net.IsOwner is true offline, so single-player is unaffected.
        if (!Net.IsOwner(Self))
        {
            return;
        }

        ReportNameOnce();

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

        AnimIndex = !grounded ? AnimIndexJump : move != 0.0f ? AnimIndexRun : AnimIndexIdle;

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

    // ── Networking ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Client -> host: adopt the name this player chose on the connect screen.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This lives on the player rather than on <see cref="WhisperSession"/> because
    /// the host only accepts a server RPC aimed at an entity the SENDING connection
    /// owns. The session entity is scene-placed and host-owned, so a call on it would
    /// be dropped by that ownership gate; the caller's own player is the one entity it
    /// is entitled to drive.
    /// </para>
    /// <para>
    /// <c>NetPlayer.displayName</c> is a replicated field, so the host writing it here
    /// is all that is needed - the value reaches every other client on the next
    /// snapshot with no further code.
    /// </para>
    /// </remarks>
    /// <param name="name">The chosen display name; blanks fall back to "Player".</param>
    [NetRpc(NetRpcTarget.Server)]
    public void SubmitName(string name)
    {
        string trimmed = (name ?? string.Empty).Trim();
        Net.SetPlayerName(Self, trimmed.Length == 0 ? "Player" : trimmed);
    }

    /// <summary>Tell the host who we are, once, as soon as the call can be routed.
    /// Retried every frame until it is: this entity needs a live net id before the
    /// call has anything to address on the far end, and the id arrives with the
    /// spawn that created it, which may not have landed on the first tick.</summary>
    private void ReportNameOnce()
    {
        if (_nameReported || !Net.IsClient)
        {
            // The host sets its own player's name directly (WhisperSession), and
            // offline there is nobody to tell.
            return;
        }
        _nameReported = Net.Call(Self, nameof(SubmitName), WhisperSession.LocalPlayerName);
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
}
