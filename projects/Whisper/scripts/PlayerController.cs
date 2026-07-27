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

    private Vector3 _spawn;
    private float _sinceGrounded = 99.0f;
    private float _sinceJumpPressed = 99.0f;
    private bool _jumpCutDone;
    private bool _nameReported;

    // The current input command, written ONLY by ApplyInput - never read from the
    // keyboard by the simulation below. That indirection is the whole point: the
    // owner fills these from its own keys, the host fills them from the packet the
    // owner sent, and both then run the identical movement code.
    private float _move;
    private bool _jumpHeld;
    private bool _holdingDown;

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
    }

    public override void OnUpdate(float deltaTime)
    {
        // Every peer runs this script on every player entity, including other
        // people's. WHO SIMULATES is authority, not ownership: the host simulates
        // every player (from the input each owner submits), a client simulates only
        // the one it owns (that is the prediction), and a client's copy of somebody
        // else's player is driven purely by replication. Net.HasAuthority is true
        // offline, so single-player is unaffected.
        if (!Net.HasAuthority(Self))
        {
            return;
        }

        // Only the owner has a keyboard to read. Net.SendInput applies the payload
        // here immediately AND ships it to the host, so the two lines below are the
        // entire client->host input path this game has to write.
        if (Net.IsOwner(Self))
        {
            ReportNameOnce();
            Net.SendInput(Self, nameof(ApplyInput), SampleInput());
        }

        // Frozen by a pause: scripts still tick at dt=0, so skip movement entirely
        // (SampleInput above already reported neutral input to the host).
        if (Time.IsPaused)
        {
            return;
        }

        float move = _move;

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
        // The press itself arrives through ApplyInput, which zeroes this - the jump
        // buffer IS the mechanism that lets one input packet outlive the frame it
        // landed on, which is exactly what the host needs.
        _sinceJumpPressed += deltaTime;

        if (grounded)
        {
            _jumpCutDone = false;
        }
        if (_sinceJumpPressed < JumpBuffer && _sinceGrounded < CoyoteTime)
        {
            bool holdingDown = _holdingDown;
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
        if (!_jumpCutDone && velocity.Y > 0.0f && !_jumpHeld)
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

    // ── Chat ────────────────────────────────────────────────────────────────────

    /// <summary>Whether this player's own chat box currently has the keyboard.</summary>
    /// <remarks>
    /// Looked up per frame rather than cached in <see cref="OnAttach"/> for the same
    /// reason <see cref="NetPlayerSync"/> does it: scripts on an entity attach one at a
    /// time in list order, so a sibling is only guaranteed to be live from OnUpdate
    /// onward. A player prefab without a <see cref="ChatBox"/> simply never types.
    /// </remarks>
    private bool IsTypingInChat() => GetScript<ChatBox>() is { IsTyping: true };

    // ── Input ───────────────────────────────────────────────────────────────────

    /// <summary>
    /// This frame's keyboard state, encoded for the wire. Four characters: facing
    /// (L/N/R), jump pressed, jump held, down held.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Neutral rather than nothing while paused or typing. Velocity persists in the
    /// physics body, so a run in progress when the chat box opens would coast at full
    /// speed for the whole message - on the host as much as here. Reporting "no keys"
    /// is what stops it in both places; a suppressed submission would leave the host
    /// running on the last packet it got.
    /// </para>
    /// <para>
    /// The chat box owns the keyboard while it is open: without that check the letters
    /// of a message double as movement keys, and typing "add" runs the character
    /// across the arena and jumps.
    /// </para>
    /// </remarks>
    private string SampleInput()
    {
        if (Time.IsPaused || IsTypingInChat())
        {
            return "N000";
        }

        float move = 0.0f;
        if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { move -= 1.0f; }
        if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { move += 1.0f; }

        bool jumpPressed = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up);
        bool jumpHeld = Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up);
        bool down = Input.IsKeyDown(Key.S) || Input.IsKeyDown(Key.Down);

        return string.Concat(
            move < 0.0f ? "L" : move > 0.0f ? "R" : "N",
            jumpPressed ? "1" : "0",
            jumpHeld ? "1" : "0",
            down ? "1" : "0");
    }

    /// <summary>
    /// Owner -> host: this is what I am pressing. Runs on the owner immediately
    /// (prediction) and on the host when the packet lands, through the identical
    /// <see cref="Net.SendInput"/> call - so there is one movement implementation,
    /// not a local one and a remote one.
    /// </summary>
    /// <remarks>
    /// It only records the command; <see cref="OnUpdate"/> does the moving. That split
    /// is what keeps the two peers in step: the coyote/jump-buffer timers advance once
    /// per FRAME on whichever peer is simulating, while input arrives at whatever rate
    /// the framework paces it to. Folding the state machine in here would make the
    /// host's timers run at packet rate instead of frame rate.
    /// </remarks>
    [NetRpc(NetRpcTarget.Server)]
    public void ApplyInput(string payload)
    {
        if (payload is not { Length: >= 4 })
        {
            return;
        }
        _move = payload[0] == 'L' ? -1.0f : payload[0] == 'R' ? 1.0f : 0.0f;
        if (payload[1] == '1')
        {
            // The press is an EDGE, and one packet is all that carries it - so it goes
            // into the jump buffer rather than a bool the next frame would clear.
            _sinceJumpPressed = 0.0f;
        }
        _jumpHeld = payload[2] == '1';
        _holdingDown = payload[3] == '1';
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
