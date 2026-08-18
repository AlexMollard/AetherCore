using System.Collections.Generic;

namespace AetherCore;

/// <summary>
/// Semantic input actions mapping a name to one or two physical keys and an optional
/// gamepad button, fully rebindable at runtime. Pure C# over <see cref="Input"/> and
/// <see cref="Gamepad"/> - the action table is process-global, so <see cref="Register"/>
/// is an idempotent upsert safe to call every attach.
///
/// <para>Binding a gamepad button here rather than testing it at each call site is what
/// keeps "jump" one concept: the button and the key answer the same question, and a
/// rebinding screen has one table to edit. Analogue sticks are not actions - read
/// <see cref="Gamepad.LeftStick"/> directly, since a direction is not a yes/no.</para>
/// </summary>
public static class InputActions
{
    private static readonly Dictionary<string, (Key Primary, Key Secondary, GamepadButton? Pad)> Actions = new();

    /// <summary>Bind (or rebind) an action to a primary key, an optional secondary key,
    /// and an optional gamepad button. Any of the three satisfies the action.</summary>
    public static void Register(string name, Key primary, Key secondary = Key.None, GamepadButton? gamepad = null)
    {
        Actions[name] = (primary, secondary, gamepad);
    }

    public static bool IsPressed(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyPressed(a.Primary)
            || (a.Secondary != Key.None && Input.IsKeyPressed(a.Secondary))
            || (a.Pad.HasValue && Gamepad.IsPressed(a.Pad.Value));
    }

    public static bool IsDown(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyDown(a.Primary)
            || (a.Secondary != Key.None && Input.IsKeyDown(a.Secondary))
            || (a.Pad.HasValue && Gamepad.IsDown(a.Pad.Value));
    }

    public static bool IsReleased(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyReleased(a.Primary)
            || (a.Secondary != Key.None && Input.IsKeyReleased(a.Secondary))
            || (a.Pad.HasValue && Gamepad.IsReleased(a.Pad.Value));
    }

    /// <summary>Register the default WASD + time-of-day + toy bindings.</summary>
    public static void RegisterDefaults()
    {
        Register("toggle_rotate", Key.Space);
        Register("speed_up", Key.Equal);
        Register("speed_down", Key.Minus);
        Register("toggle_manual", Key.T);
        Register("time_up", Key.I);
        Register("time_down", Key.J);
        Register("time_noon", Key.G);
        Register("time_midnight", Key.O);
        Register("move_forward", Key.W);
        Register("move_backward", Key.S);
        Register("move_left", Key.A);
        Register("move_right", Key.D);
    }
}
