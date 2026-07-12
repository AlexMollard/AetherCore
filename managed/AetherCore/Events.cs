using System;
using System.Collections.Generic;

namespace AetherCore;

/// <summary>
/// A simple global publish/subscribe event bus for decoupled messaging between
/// scripts - one system raises a named event, any number of others react without
/// referencing each other. Handlers are invoked synchronously on Publish.
/// </summary>
public static class Events
{
    private static readonly Dictionary<string, Action<object?>> s_handlers = new(StringComparer.Ordinal);

    /// <summary>Subscribe <paramref name="handler"/> to a named event.</summary>
    public static void Subscribe(string name, Action<object?> handler)
    {
        s_handlers[name] = s_handlers.TryGetValue(name, out Action<object?>? existing) ? existing + handler : handler;
    }

    /// <summary>Remove a previously-subscribed handler.</summary>
    public static void Unsubscribe(string name, Action<object?> handler)
    {
        if (s_handlers.TryGetValue(name, out Action<object?>? existing))
        {
            Action<object?>? remaining = existing - handler;
            if (remaining == null)
            {
                s_handlers.Remove(name);
            }
            else
            {
                s_handlers[name] = remaining;
            }
        }
    }

    /// <summary>Raise a named event, invoking every subscribed handler with <paramref name="data"/>.</summary>
    public static void Publish(string name, object? data = null)
    {
        if (s_handlers.TryGetValue(name, out Action<object?>? handler))
        {
            handler?.Invoke(data);
        }
    }

    /// <summary>Drop all subscriptions (e.g. on a scene change).</summary>
    public static void Clear() => s_handlers.Clear();
}
