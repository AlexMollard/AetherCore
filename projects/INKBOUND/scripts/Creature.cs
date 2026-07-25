using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Registry of things ink can smother, and things that notice it being used.
///
/// INKBOUND has no separate combat verb - you kill the way you travel, by drawing. A creature
/// registers here on attach; when a stroke is laid across it, <see cref="AetherInk"/> asks the
/// registry to unmake it. Registering rather than type-testing keeps each creature in charge of its
/// own death, and lets a new enemy join the system with one line in OnAttach.
///
/// Smothered creatures are NOT forgotten - they come back when the wanderer next comes apart. Your
/// drawn ink survives your death and the cave's teeth do too, so a level never quietly empties out
/// into a stroll just because you kept dying in it.
/// </summary>
public static class Creature
{
    private sealed class Entry
    {
        public Action Kill = () => { };
        public Action Revive = () => { };
        public Action<Vector2>? Alert;
        public bool Dead;
    }

    private static readonly Dictionary<uint, Entry> s_all = new();

    /// <summary>Announce a creature: how it dies, how it comes back, and (optionally) how it reacts
    /// to ink being drawn nearby.</summary>
    public static void Register(uint id, Action onSmothered, Action onRevived, Action<Vector2>? onInkNearby = null)
        => s_all[id] = new Entry { Kill = onSmothered, Revive = onRevived, Alert = onInkNearby };

    public static void Unregister(uint id) => s_all.Remove(id);

    /// <summary>Smother this entity if it is a living creature. True if something died.</summary>
    public static bool TrySmother(uint id)
    {
        if (!s_all.TryGetValue(id, out Entry? e) || e.Dead) { return false; }
        e.Dead = true;
        e.Kill();
        return true;
    }

    /// <summary>Everything smothered this run gets back up. Called when the player dies.</summary>
    public static void ReviveAll()
    {
        foreach (Entry e in s_all.Values)
        {
            if (!e.Dead) { continue; }
            e.Dead = false;
            e.Revive();
        }
    }

    /// <summary>Ink was drawn here. Living creatures within earshot get to react - conjuring is not
    /// a quiet thing to do next to something that hunts.</summary>
    public static void InkDrawnAt(Vector2 p)
    {
        foreach (Entry e in s_all.Values)
        {
            if (!e.Dead) { e.Alert?.Invoke(p); }
        }
    }
}
