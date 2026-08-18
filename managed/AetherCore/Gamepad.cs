using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>
/// Standard gamepad buttons. Any controller the engine recognises reports through
/// these names whatever the vendor, so a script never branches on hardware.
/// Face-button names follow the Xbox layout; on a DualShock, <see cref="A"/> is Cross,
/// <see cref="B"/> is Circle, <see cref="X"/> is Square and <see cref="Y"/> is Triangle.
/// </summary>
public enum GamepadButton
{
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    LeftBumper = 4,
    RightBumper = 5,
    Back = 6,
    Start = 7,
    Guide = 8,
    LeftThumb = 9,
    RightThumb = 10,
    DpadUp = 11,
    DpadRight = 12,
    DpadDown = 13,
    DpadLeft = 14,
}

/// <summary>
/// Raw analogue axes. Prefer <see cref="Gamepad.Stick"/> and <see cref="Gamepad.Trigger"/>,
/// which apply a deadzone and correct two sign conventions - see
/// <see cref="Gamepad.GetAxisRaw"/> for what "raw" actually means here.
/// </summary>
public enum GamepadAxis
{
    LeftX = 0,
    LeftY = 1,
    RightX = 2,
    RightY = 3,
    LeftTrigger = 4,
    RightTrigger = 5,
}

public enum GamepadStick { Left = 0, Right = 1 }

public enum GamepadTrigger { Left = 0, Right = 1 }

/// <summary>
/// Gamepad state for the current frame.
/// <para>
/// Every call takes an optional <c>pad</c> slot and defaults to <see cref="Any"/>, meaning
/// "whichever controller is connected first". That default is deliberate: controller slots
/// are not packed, so a single pad is often not in slot 0 - a wireless receiver, a virtual
/// device or a racing wheel can hold it. A single-player game should simply never pass a
/// slot. Pass one only for local multiplayer, where the slot identifies the player.
/// </para>
/// </summary>
/// <example>
/// <code>
/// // Move and jump, controller or keyboard, without caring which:
/// float move = Gamepad.LeftStick.X + Input.GetAxisRaw(Key.A, Key.D);
/// bool jump  = Gamepad.IsPressed(GamepadButton.A) || Input.IsKeyPressed(Key.Space);
/// </code>
/// </example>
public static class Gamepad
{
    /// <summary>The first connected controller, whichever slot it landed in.</summary>
    public const int Any = -1;

    /// <summary>Whether a recognised controller is connected in this slot.</summary>
    /// <remarks>
    /// A controller the engine has no mapping for reports as *not* connected, because there
    /// is nothing useful a game can do with an unlabelled pile of axes. This is rare - the
    /// bundled mapping database covers a few hundred pads.
    /// </remarks>
    public static bool IsConnected(int pad = Any) => Native.aether_input_gamepad_connected(pad) != 0;

    /// <summary>The concrete slot <paramref name="pad"/> names, or -1 if nothing is connected.</summary>
    public static int Resolve(int pad = Any) => Native.aether_input_gamepad_resolve(pad);

    /// <summary>Controller name, e.g. "Xbox Controller". Empty when disconnected.</summary>
    public static unsafe string Name(int pad = Any)
    {
        Span<byte> buffer = stackalloc byte[128];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_input_gamepad_name(pad, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    // ── Buttons ───────────────────────────────────────────────────────────────────

    /// <summary>Whether the button is held this frame.</summary>
    public static bool IsDown(GamepadButton button, int pad = Any)
        => Native.aether_input_gamepad_button_down((int)button, pad) != 0;

    /// <summary>Whether the button went down this frame.</summary>
    public static bool IsPressed(GamepadButton button, int pad = Any)
        => Native.aether_input_gamepad_button_pressed((int)button, pad) != 0;

    /// <summary>Whether the button came up this frame. A controller unplugged while the
    /// button was held does NOT fire this - it simply stops reporting, and every button
    /// reads false from that frame on. Watch <see cref="IsConnected"/> for that case.</summary>
    public static bool IsReleased(GamepadButton button, int pad = Any)
        => Native.aether_input_gamepad_button_released((int)button, pad) != 0;

    // ── Sticks ────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Stick direction, deadzoned, with <c>Y</c> positive upwards to match world space.
    /// Magnitude runs 0 at the deadzone edge to 1 at full deflection, in every direction
    /// equally - so a diagonal push is not faster than a straight one.
    /// </summary>
    public static Vector2 Stick(GamepadStick stick, int pad = Any)
        => Native.aether_input_gamepad_stick((int)stick, pad);

    /// <summary>Left stick. Zero when no controller is connected, so this is safe to read unconditionally.</summary>
    public static Vector2 LeftStick => Stick(GamepadStick.Left);

    /// <summary>Right stick. Zero when no controller is connected.</summary>
    public static Vector2 RightStick => Stick(GamepadStick.Right);

    // ── Triggers ──────────────────────────────────────────────────────────────────

    /// <summary>Trigger pull from 0 (released) to 1 (fully pressed), past a small deadzone.</summary>
    public static float Trigger(GamepadTrigger trigger, int pad = Any)
        => Native.aether_input_gamepad_trigger((int)trigger, pad);

    /// <summary>Left trigger, 0 to 1.</summary>
    public static float LeftTrigger => Trigger(GamepadTrigger.Left);

    /// <summary>Right trigger, 0 to 1.</summary>
    public static float RightTrigger => Trigger(GamepadTrigger.Right);

    // ── Raw ───────────────────────────────────────────────────────────────────────

    /// <summary>
    /// The untouched axis value in -1..1: no deadzone, no deadzone rescale, Y still
    /// negative-is-up, and triggers still resting at -1 rather than 0. Use this only to
    /// build a custom response curve; <see cref="Stick"/> and <see cref="Trigger"/> are
    /// what a game normally wants.
    /// </summary>
    public static float GetAxisRaw(GamepadAxis axis, int pad = Any)
        => Native.aether_input_gamepad_axis_raw((int)axis, pad);

    /// <summary>
    /// Override the deadzones applied by <see cref="Stick"/> and <see cref="Trigger"/>.
    /// Defaults are 0.24 and 0.12, matching the thresholds controller hardware is built
    /// around. Raise the stick value for a worn pad that drifts at rest. Values are
    /// clamped below 1 - a deadzone of 1 would leave no usable travel.
    /// </summary>
    public static void SetDeadzones(float stick, float trigger)
        => Native.aether_input_gamepad_set_deadzones(stick, trigger);
}
