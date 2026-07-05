using System.Collections.Generic;

namespace AetherCore.Managed;

/// <summary>
/// Semantic input actions mapping a name to one or two physical keys, fully
/// rebindable at runtime. Ported from the old input_bindings.das. Pure C# over
/// <see cref="Input"/> — the action table is process-global (like the das one),
/// so <see cref="Register"/> is an idempotent upsert safe to call every attach.
/// </summary>
public static class InputActions
{
    private static readonly Dictionary<string, (Key Primary, Key Secondary)> Actions = new();

    /// <summary>Bind (or rebind) an action to a primary and optional secondary key.</summary>
    public static void Register(string name, Key primary, Key secondary = Key.None)
    {
        Actions[name] = (primary, secondary);
    }

    public static bool IsPressed(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyPressed(a.Primary) || (a.Secondary != Key.None && Input.IsKeyPressed(a.Secondary));
    }

    public static bool IsDown(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyDown(a.Primary) || (a.Secondary != Key.None && Input.IsKeyDown(a.Secondary));
    }

    public static bool IsReleased(string name)
    {
        if (!Actions.TryGetValue(name, out var a))
        {
            return false;
        }
        return Input.IsKeyReleased(a.Primary) || (a.Secondary != Key.None && Input.IsKeyReleased(a.Secondary));
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
