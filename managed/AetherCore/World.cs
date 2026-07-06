using System;
using System.Text;

namespace AetherCore;

/// <summary>Entity creation and world-level queries. Expanded in later phases.</summary>
public static unsafe class World
{
    /// <summary>Creates a new entity (named "Entity", tracked as scene-owned).</summary>
    public static Entity Create() => new(Native.aether_entity_create());

    public static bool IsValid(Entity entity) => Native.aether_entity_valid(entity.Id) != 0;

    /// <summary>Cache a primitive mesh ("cube","sphere","plane","quad","triangle").</summary>
    public static MeshHandle CreateMesh(string kind) => new(Native.aether_create_mesh(kind));

    /// <summary>True if a scene file with this name exists on disk.</summary>
    public static bool SceneFileExists(string name) => Native.aether_scene_file_exists(name) != 0;

    /// <summary>
    /// Fills <paramref name="buffer"/> with entities that have a transform and
    /// returns the count written (truncated to the buffer size).
    /// </summary>
    public static int GetEntitiesWithTransform(Span<Entity> buffer)
    {
        if (buffer.IsEmpty)
        {
            return 0;
        }
        fixed (Entity* ptr = buffer)
        {
            return Native.aether_world_get_entities_with_transform((uint*)ptr, buffer.Length);
        }
    }

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
