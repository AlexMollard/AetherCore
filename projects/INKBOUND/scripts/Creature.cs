using System;
using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Registry of things ink can smother. INKBOUND has no separate combat verb - you kill the way you
/// travel, by drawing. A creature registers itself here on attach; when a stroke is laid across it,
/// <see cref="AetherInk"/> asks this registry to unmake it.
///
/// Registering rather than type-testing keeps each creature in charge of its own death animation,
/// and lets a new enemy join the system by adding one line to its OnAttach.
/// </summary>
public static class Creature
{
    private static readonly Dictionary<uint, Action> s_smother = new();

    /// <summary>Announce that this entity can be smothered, and how it dies.</summary>
    public static void Register(uint id, Action onSmothered) => s_smother[id] = onSmothered;

    public static void Unregister(uint id) => s_smother.Remove(id);

    /// <summary>Smother this entity if it is a creature. Returns true if something died.</summary>
    public static bool TrySmother(uint id)
    {
        if (!s_smother.TryGetValue(id, out Action? kill)) { return false; }
        s_smother.Remove(id); // never smother the same body twice
        kill();
        return true;
    }
}
