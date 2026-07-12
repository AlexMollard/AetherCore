using System.Numerics;

namespace AetherCore;

/// <summary>
/// Scene-level entity creation and lookup - the Unity-style entry points for
/// building and finding scene content from script. Structural changes made here
/// run on the game thread and are safe against the render thread.
/// </summary>
public static class Scene
{
    /// <summary>Spawn a new empty entity with a name and a transform at <paramref name="position"/>.</summary>
    public static Entity Create(string name, Vector3 position = default)
        => new(Native.aether_scene_create_entity(name, position));

    /// <summary>The first entity with this exact name, or an invalid entity.</summary>
    public static Entity Find(string name)
        => new(Native.aether_scene_find_by_name(name));

    /// <summary>The first entity carrying <paramref name="tag"/>, or an invalid entity.</summary>
    public static Entity FindWithTag(string tag)
    {
        TagId id = Tags.Find(tag);
        if (!id.IsValid)
        {
            return default;
        }
        System.Span<Entity> one = stackalloc Entity[1];
        return Tags.GetEntitiesWith(id, one) > 0 ? one[0] : default;
    }

    /// <summary>
    /// Instantiate a prefab (a <c>.prefab.toml</c> asset) into the live scene,
    /// placing its root at <paramref name="position"/>. Returns the root entity, or
    /// an invalid entity if the prefab is missing or empty.
    /// </summary>
    public static Entity Instantiate(string prefabName, Vector3 position = default)
        => new(Native.aether_scene_instantiate_prefab(prefabName, position));
}
