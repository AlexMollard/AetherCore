using System.Collections.Generic;

namespace AetherCore;

/// <summary>
/// The live <see cref="EntityScript"/> instances of the running world, indexed by
/// entity. This is what <see cref="Entity.GetScript{T}"/> (and the
/// <see cref="EntityScript.GetScript{T}"/> shorthand) reads.
///
/// The table is maintained entirely in managed code, from the same two calls that
/// already bracket an instance's lifetime: <c>AetherCore.Interop.ScriptRegistry</c>
/// registers on CreateInstance (right after <see cref="EntityScript.Bind"/>) and
/// unregisters on DestroyInstance (right before the GCHandle is freed). There is no
/// separate native query - the native ScriptComponentSystem's handle table and this
/// one are populated and drained by the exact same pair of calls, so they cannot
/// drift, and an entity id can never resolve to an instance from a previous entity
/// that happened to reuse the id.
///
/// Everything here runs on the engine's main loop thread (the script system is
/// driven from World update), so no locking is needed.
/// </summary>
internal static class ScriptInstances
{
    // Entity id -> that entity's live instances, in attach order. Small lists
    // (an entity has a handful of scripts at most), so a linear scan beats any
    // per-type index and keeps "first attached wins" trivially true.
    private static readonly Dictionary<uint, List<EntityScript>> s_byEntity = new();

    /// <summary>Record a freshly created instance as live on <paramref name="entityId"/>.</summary>
    internal static void Register(uint entityId, EntityScript script)
    {
        if (!s_byEntity.TryGetValue(entityId, out List<EntityScript>? scripts))
        {
            scripts = new List<EntityScript>(2);
            s_byEntity[entityId] = scripts;
        }
        scripts.Add(script);
    }

    /// <summary>Drop an instance that is being destroyed. Safe to call for an
    /// instance that was never registered.</summary>
    internal static void Unregister(EntityScript script)
    {
        uint entityId = script.Self.Id;
        if (!s_byEntity.TryGetValue(entityId, out List<EntityScript>? scripts))
        {
            return;
        }
        scripts.Remove(script);
        if (scripts.Count == 0)
        {
            // Drop the bucket so an entity that loses all its scripts leaves no
            // entry behind to be inherited by a later entity with the same id.
            s_byEntity.Remove(entityId);
        }
    }

    /// <summary>The first live instance of <typeparamref name="T"/> on
    /// <paramref name="entityId"/>, or null if there is none.</summary>
    internal static T? Find<T>(uint entityId) where T : EntityScript
    {
        if (!s_byEntity.TryGetValue(entityId, out List<EntityScript>? scripts))
        {
            return null;
        }
        for (int i = 0; i < scripts.Count; i++)
        {
            if (scripts[i] is T match)
            {
                return match;
            }
        }
        return null;
    }

    /// <summary>Forget every instance. Called when the script assembly is torn
    /// down: the table holds strong references into the collectible load context,
    /// so it must be empty before that context can unload.</summary>
    internal static void Clear() => s_byEntity.Clear();
}
