using System;
using System.Text;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>Entity creation and world-level queries. Expanded in later phases.</summary>
public static unsafe class World
{
    /// <summary>Creates a new entity (named "Entity", tracked as scene-owned).</summary>
    public static Entity Create() => new(Native.aether_entity_create());

    public static bool IsValid(Entity entity) => Native.aether_entity_valid(entity.Id) != 0;

    internal static string GetName(uint id)
    {
        Span<byte> buffer = stackalloc byte[256];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_get_name(id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }
}
