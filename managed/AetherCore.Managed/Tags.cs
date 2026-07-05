using System;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>A dynamic tag id (0xFFFFFFFF is invalid / not found).</summary>
public readonly record struct TagId(uint Value)
{
    public bool IsValid => Value != 0xFFFFFFFF;
}

/// <summary>Dynamic entity tags — register a name once, then tag/query entities.</summary>
public static unsafe class Tags
{
    /// <summary>Register (or fetch) a tag by name.</summary>
    public static TagId Create(string name) => new(Native.aether_tag_create(name));

    /// <summary>Look up an existing tag by name (invalid if unknown).</summary>
    public static TagId Find(string name) => new(Native.aether_tag_get_id(name));

    public static void Add(Entity entity, TagId tag) => Native.aether_tag_add(entity.Id, tag.Value);

    public static bool Has(Entity entity, TagId tag) => Native.aether_tag_has(entity.Id, tag.Value) != 0;

    public static void Remove(Entity entity, TagId tag) => Native.aether_tag_remove(entity.Id, tag.Value);

    /// <summary>
    /// Fills <paramref name="buffer"/> with entities carrying <paramref name="tag"/>
    /// and returns the count written (truncated to the buffer size).
    /// </summary>
    public static int GetEntitiesWith(TagId tag, Span<Entity> buffer)
    {
        if (buffer.IsEmpty)
        {
            return 0;
        }
        fixed (Entity* ptr = buffer)
        {
            // Entity is a single uint field, so its storage is layout-compatible
            // with a uint buffer.
            return Native.aether_tag_get_entities(tag.Value, (uint*)ptr, buffer.Length);
        }
    }
}
