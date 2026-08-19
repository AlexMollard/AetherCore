using AetherCore;

/// <summary>
/// INKBOUND's whole control scheme, in one place.
///
/// <para>Every script asks this what the player wants ("is jump pressed") rather than which
/// key is down, so a binding lives at exactly one site. That matters more than it looks:
/// jump is four inputs and is read from three places, and the version of this file where
/// each site tested its own keys had the gamepad wired into two of them and not the third,
/// which reads as "the jump button sometimes doesn't work".</para>
///
/// <para>Kept in the project rather than the SDK because a control scheme is a game design
/// decision. The engine's <see cref="InputActions"/> covers the simple case of one name over
/// one or two keys plus a pad button; several of these need more than that - an analogue
/// axis, a three-key jump, a stick that has to behave like a d-pad - so they are spelled out
/// here instead of bent to fit.</para>
///
/// <para>Pad layout: <b>A</b> jump/confirm, <b>B</b> cancel, <b>X</b> interact, <b>Y</b> hold
/// to commit, <b>Start</b> pause, <b>Back</b> quit to menu, stick and d-pad move.</para>
/// </summary>
public static class Controls
{
    /// <summary>How far the stick must lean before it counts as a menu press.</summary>
    private const float NavThreshold = 0.5f;

    /// <summary>How far the stick must lean before it counts as "holding down".</summary>
    private const float DownThreshold = 0.5f;

    // ── Movement ────────────────────────────────────────────────────────────────

    /// <summary>
    /// Horizontal movement, -1 to 1. Keyboard and d-pad are all-or-nothing; the stick is
    /// analogue, so a gentle lean walks rather than runs.
    /// </summary>
    public static float MoveX
    {
        get
        {
            float keys = 0.0f;
            if (Input.IsKeyDown(Key.A) || Input.IsKeyDown(Key.Left)) { keys -= 1.0f; }
            if (Input.IsKeyDown(Key.D) || Input.IsKeyDown(Key.Right)) { keys += 1.0f; }
            // Keyboard wins while it is held. Otherwise a controller resting a hair off
            // centre would fight the keys, and the player would feel a drift they cannot
            // see the cause of.
            if (keys != 0.0f) { return keys; }

            if (Gamepad.IsDown(GamepadButton.DpadLeft)) { return -1.0f; }
            if (Gamepad.IsDown(GamepadButton.DpadRight)) { return 1.0f; }
            // Already deadzoned by the engine, so a resting stick is exactly zero here.
            return Gamepad.LeftStick.X;
        }
    }

    /// <summary>Holding "down" - crouch-style intent, used with jump to drop through platforms.</summary>
    public static bool DownHeld =>
        Input.IsKeyDown(Key.S) || Input.IsKeyDown(Key.Down)
        || Gamepad.IsDown(GamepadButton.DpadDown)
        || Gamepad.LeftStick.Y < -DownThreshold;

    // ── Jump ────────────────────────────────────────────────────────────────────

    /// <summary>Jump was requested this frame. Feeds the jump buffer.</summary>
    public static bool JumpPressed =>
        Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.W) || Input.IsKeyPressed(Key.Up)
        || Gamepad.IsPressed(GamepadButton.A);

    /// <summary>
    /// Jump is still held. Distinct from <see cref="JumpPressed"/> and not derivable from it:
    /// releasing early is what cuts the ascent short, so variable jump height depends on this
    /// tracking exactly the same set of inputs the press does.
    /// </summary>
    public static bool JumpHeld =>
        Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W) || Input.IsKeyDown(Key.Up)
        || Gamepad.IsDown(GamepadButton.A);

    // ── Actions ─────────────────────────────────────────────────────────────────

    /// <summary>Holding the "give yourself to the dark" input. A deliberate hold, so it sits
    /// on a face button well away from jump.</summary>
    public static bool CommitHeld =>
        Input.IsKeyDown(Key.Q) || Gamepad.IsDown(GamepadButton.Y);

    /// <summary>Talk to whatever is in reach.</summary>
    public static bool InteractPressed =>
        Input.IsKeyPressed(Key.E) || Gamepad.IsPressed(GamepadButton.X);

    /// <summary>Toggle the pause menu.</summary>
    public static bool PausePressed =>
        Input.IsKeyPressed(Key.Escape) || Input.IsKeyPressed(Key.P)
        || Gamepad.IsPressed(GamepadButton.Start);

    /// <summary>Confirm, advance a line of dialogue, take the highlighted choice.</summary>
    public static bool AdvancePressed =>
        Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.Enter)
        || Gamepad.IsPressed(GamepadButton.A);

    /// <summary>Back out of whatever is open.</summary>
    public static bool CancelPressed =>
        Input.IsKeyPressed(Key.Escape) || Gamepad.IsPressed(GamepadButton.B);

    /// <summary>Abandon the run and return to the menu.</summary>
    public static bool ReturnToMenuPressed =>
        Input.IsKeyPressed(Key.M) || Gamepad.IsPressed(GamepadButton.Back);

    // ── Menu navigation ─────────────────────────────────────────────────────────

    /// <summary>Move the highlight up one entry.</summary>
    public static bool MenuUpPressed =>
        Input.IsKeyPressed(Key.Up) || Input.IsKeyPressed(Key.W)
        || Gamepad.IsPressed(GamepadButton.DpadUp)
        || StickNavUp;

    /// <summary>Move the highlight down one entry.</summary>
    public static bool MenuDownPressed =>
        Input.IsKeyPressed(Key.Down) || Input.IsKeyPressed(Key.S)
        || Gamepad.IsPressed(GamepadButton.DpadDown)
        || StickNavDown;

    // A stick has no press edge of its own - it is just a position, so a held stick would
    // scroll a menu at one entry per frame. These latch it: the edge fires once when the
    // stick crosses the threshold and not again until it comes back inside.
    //
    // Recomputed at most once per frame and cached, so reading MenuUpPressed twice in the
    // same frame gives the same answer both times, the way a real key press does. Frame
    // count is used rather than elapsed time because it keeps advancing while the game is
    // paused, which is exactly when a menu is on screen.
    private static long _navFrame = -1;
    private static bool _navUpEdge;
    private static bool _navDownEdge;
    private static bool _navUpLatched;
    private static bool _navDownLatched;

    private static bool StickNavUp
    {
        get { RefreshStickNav(); return _navUpEdge; }
    }

    private static bool StickNavDown
    {
        get { RefreshStickNav(); return _navDownEdge; }
    }

    private static void RefreshStickNav()
    {
        long frame = Time.FrameCount;
        if (frame == _navFrame) { return; }
        _navFrame = frame;

        float y = Gamepad.LeftStick.Y;
        bool up = y > NavThreshold;
        bool down = y < -NavThreshold;

        _navUpEdge = up && !_navUpLatched;
        _navDownEdge = down && !_navDownLatched;
        _navUpLatched = up;
        _navDownLatched = down;
    }
}
