namespace AetherCore;

/// <summary>
/// Whether an entity's mesh actually draws. Both stay true by default - a ragdoll's
/// capsule/box bones and, once <see cref="Ragdoll.DriveSkin"/> is called, its driven
/// skinned mesh - so toggle one off here to choose which is actually seen without
/// touching physics or animation at all.
/// </summary>
public static class MeshRenderer
{
    public static bool GetVisible(Entity entity) => Native.aether_mesh_renderer_get_visible(entity.Id) != 0;
    public static void SetVisible(Entity entity, bool visible) => Native.aether_mesh_renderer_set_visible(entity.Id, visible ? 1 : 0);
}
