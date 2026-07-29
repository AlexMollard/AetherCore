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

    /// <summary>
    /// The clip that movement/physics state currently calls for - one of the
    /// AnimIndex* constants above. Computed here every frame (this script already
    /// owns the grounded/movement state that decides it) but NOT applied to the
    /// sprite here; NetPlayerSync reads it, replicates it, and is the one place
    /// that calls SpriteAnimator, so the owner and every remote copy switch clips
    /// through the identical path instead of two scripts guessing independently.
    /// </summary>
    public int AnimIndex { get; private set; }

    /// <summary>
    /// Which way the character is facing - true while it last moved left. Decided
    /// here every frame, for exactly the reason <see cref="AnimIndex"/> is (this
    /// script owns the movement state), and applied to the sprite nowhere here:
    /// <see cref="NetPlayerSync"/> reads it, replicates it, and is the one place that
    /// calls <see cref="SpriteRenderer.SetFlipX"/>, so the owner's own sprite and
    /// every remote copy flip through the identical path.
    /// </summary>
    /// <remarks>
    /// This method returns early for anything this peer does not own, so a facing
    /// applied here would never run on a remote copy at all - which is exactly why
    /// remote players used to moonwalk. Replicating the decision, rather than
    /// re-deriving it from a position delta on each peer, is the same choice
    /// <see cref="AnimIndex"/> makes and for the same reason: one decider, one apply.
    /// </remarks>
    public bool FacingLeft { get; private set; }

    /// <summary>Where this player belongs - the spawn marker it was created at.</summary>
    /// <remarks>
    /// Read by <see cref="PlayerCombat"/> so a death and a fall out of the world put
    /// the character back in the same place. One capture, one answer: a second script
    /// noting its own idea of "home" is a second thing that can end up somewhere else.
    /// </remarks>
    public Vector3 SpawnPoint { get; private set; }

    private float _sinceGrounded = 99.0f;
    private float _sinceJumpPressed = 99.0f;
    private bool _jumpCutDone;

    // Squash & stretch is purely visual (drives the sprite quad size, never the
    // collider): stretch while airborne, squash impulse on landing, eased back.
    private Vector2 _baseSpriteSize = new(32.0f, 32.0f);
    private float _squash;       // +squashed (wide/short), -stretched (tall/thin)
    private bool _wasGrounded = true;

    public override void OnAttach()
    {
        SpawnPoint = Self.Position;
        _baseSpriteSize = SpriteRenderer.GetPixelSize(Self);
        Physics2D.EnableEvents(Self);
        Physics2D.SetGravityScale(Self, GravityScale);
    }

    public override void OnUpdate(float deltaTime)
    {
        // Every peer runs this script on every player entity, including other
        // people's, so this gate is what stops four machines simulating the same
        // character four different ways. WHO SIMULATES IS WHO OWNS: the owner runs
        // the movement below and the framework replicates the result, and every
        // other peer's copy of this player is driven purely by replication.
        // Net.HasAuthority is true offline, so single-player is unaffected.
        if (!Net.HasAuthority(Self))
        {
            return;
        }

        ClaimName();

        // Frozen by a pause: scripts still tick at dt=0, so skip movement entirely.
        // The body keeps whatever velocity it had, which is correct - nothing else
        // is deciding this character's state.
        if (Time.IsPaused)
        {
            return;
        }

        // Dead players do not run, jump or drop through platforms. Returning here is
        // the whole of it: PlayerCombat is what holds the corpse at its spawn point and
        // what brings it back, so there is one place that knows what being dead means
        // and this one only has to stop. The animation is pinned to idle so a body that
        // died mid-stride is not left running on the spot on every other peer's screen.
        if (GetScript<PlayerCombat>() is { IsAlive: false })
        {
            AnimIndex = AnimIndexIdle;
            return;
        }

        // Read straight off the keyboard. There is no input packet and no handler in
        // between any more: this peer owns the character, simulates it here, and what
        // it simulates IS what the other peers are shown.
        float move = 0.0f;
        bool jumpPressed = false;
        bool jumpHeld = false;
        bool holdingDown = false;
        // The UI owns the keyboard while anything in it is focused - a chat message being
        // typed, a pause menu being read. Without this check the letters of a message
        // double as movement keys, and typing "add" runs the character across the arena
        // and jumps.
        //
        // Asked of the ENGINE rather than of a sibling script. Focus is committed by the
        // UI pass before the first script of the frame runs, so every script gets the same
        // answer whatever order they update in; a "somebody is typing" flag published by
        // one script for another to read is only right when the two happen to tick in the
        // right order, and getting that wrong is what used to make one Escape both cancel
        // a message and quit the session.
        if (!Ui.HasFocus)
        {
            if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
            if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }
            jumpPressed = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up);
            jumpHeld = Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up);
            holdingDown = Input.IsKeyDown(Key.S) || Input.IsKeyDown(Key.Down);
        }
        if (jumpPressed)
        {
            _sinceJumpPressed = 0.0f;
        }

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

        if (grounded)
        {
            _jumpCutDone = false;
        }
        if (_sinceJumpPressed < JumpBuffer && _sinceGrounded < CoyoteTime)
        {
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
        if (!_jumpCutDone && velocity.Y > 0.0f && !jumpHeld)
        {
            velocity.Y *= JumpCutFactor;
            _jumpCutDone = true;
        }

        Physics2D.SetLinearVelocity(Self, velocity);

        // Facing is DECIDED here and APPLIED nowhere here - see FacingLeft. Calling
        // SetFlipX in this branch is what left every remote copy moonwalking: this
        // whole method is owner-only, so a remote copy never reached the call.
        if (move != 0.0f)
        {
            FacingLeft = move < 0.0f;
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
    /// Write this player's chosen display name onto its own entity, and let
    /// replication carry it outward like any other owned state.
    /// </summary>
    /// <remarks>
    /// <para>
    /// THE OWNER AUTHORS ITS OWN NAME. This method only ever runs on the peer that
    /// owns this player - <see cref="OnUpdate"/> has already returned for anybody
    /// else - so the write lands on the one machine entitled to make it, and
    /// <c>NetPlayer.displayName</c> reaches every other peer on the next snapshot with
    /// no further code.
    /// </para>
    /// <para>
    /// This used to be a <c>[NetRpc(NetRpcTarget.Server)] SubmitName</c> that asked the
    /// HOST to write the field, which was correct under the old host-authoritative
    /// model and is a bug under this one: the client then replicated its own copy of
    /// the same entity - still carrying the empty authored name - straight back over
    /// the host's write, and whether a name survived was a race. Some players' names
    /// showed and some did not, per session, for that reason.
    /// </para>
    /// <para>
    /// Called every frame rather than once, because <see cref="Net.ClaimPlayerName"/>
    /// is not just a write: it steps around a name a player ahead of us already has
    /// ("Alice" becomes "Alice (2)"), and the player it has to step around may not have
    /// arrived yet when this player spawns. Re-resolving from the DESIRED name each
    /// frame settles on the right answer as the session fills up, and costs a walk of
    /// at most four players.
    /// </para>
    /// </remarks>
    private void ClaimName() => Net.ClaimPlayerName(Self, NetSession.LocalPlayerName);

    /// <summary>Snap back to the entity's starting position after falling out of
    /// the world. There are no checkpoints in this build - it is always the spawn point.</summary>
    private void Respawn()
    {
        Self.Position = SpawnPoint;
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
