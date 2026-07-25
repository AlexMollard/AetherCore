using System.Collections.Generic;

namespace AetherGame;

/// <summary>
/// The wiring between puzzle pieces. Anything can emit on a named channel - a pressure plate holding
/// weight, an ink conduit whose circuit is closed - and anything can listen, chiefly
/// <see cref="Gate"/>. A channel is on while at least one emitter says so, so two plates can hold one
/// gate, or a plate and a circuit can offer alternative solutions to the same door.
///
/// Emitters key by their own entity id, so a piece that disappears (or a scene that unloads) cannot
/// leave a channel stuck on.
/// </summary>
public static class Signal
{
    private static readonly Dictionary<string, HashSet<uint>> s_on = new();

    /// <summary>Emitter <paramref name="id"/> asserts (or drops) <paramref name="channel"/>.</summary>
    public static void Set(string channel, uint id, bool on)
    {
        if (string.IsNullOrEmpty(channel)) { return; }
        if (!s_on.TryGetValue(channel, out HashSet<uint>? set))
        {
            if (!on) { return; }
            set = new HashSet<uint>();
            s_on[channel] = set;
        }
        if (on) { set.Add(id); } else { set.Remove(id); }
    }

    /// <summary>Is anything currently asserting this channel?</summary>
    public static bool IsOn(string channel)
        => !string.IsNullOrEmpty(channel) && s_on.TryGetValue(channel, out HashSet<uint>? set) && set.Count > 0;

    /// <summary>Drop everything - called when a level unloads so nothing carries over.</summary>
    public static void Clear() => s_on.Clear();
}
