namespace AetherCore;

/// <summary>
/// Converts an entity into a physics-driven ragdoll: roughly a dozen dynamic capsule
/// bodies (see the engine's RagdollBuilder.hpp) jointed together and matched to a
/// skeleton's bind pose by bone-name matching. The bones always render as their own
/// primitive meshes (useful on its own for seeing what the physics is doing); call
/// <see cref="DriveSkin"/> to ALSO drive a skinned mesh's real joints from those same
/// bones every frame, so the character keeps looking like itself instead of a pile of
/// boxes - see the engine's RagdollSkinDrive.hpp for exactly how.
/// </summary>
public static class Ragdoll
{
    /// <summary>
    /// Turns <paramref name="entity"/> into the ragdoll's root (pelvis) bone, keeping
    /// its id - and everything else already on it (NetworkIdentity, scripts, ...) -
    /// intact. If it carries a Character Controller (a player being knocked out or
    /// killed), the controller's last velocity seeds the ragdoll and the component is
    /// removed. <paramref name="skeletonPath"/> is a project-relative asset path (e.g.
    /// "project://assets/models/Human/Human.gltf") read purely for its bind-pose
    /// skeleton - nothing about that asset's rendering or animation is touched.
    /// </summary>
    /// <returns>Every entity the ragdoll is made of - the root first, then each other
    /// bone - or an empty array if none was built: the entity has no transform, is
    /// already a ragdoll bone, or the skeleton has no recognisable humanoid hierarchy.
    /// This is the same set the engine's RagdollComponent holds internally; a caller
    /// that needs "every entity belonging to this ragdoll" (to tag them, replicate
    /// them, or free them as a unit) reads it from here instead of reconstructing it
    /// by diffing the entities that existed before and after the call.</returns>
    public static Entity[] Spawn(Entity entity, string skeletonPath)
    {
        int count = Native.aether_ragdoll_spawn(entity.Id, skeletonPath);
        var bones = new Entity[count];
        for (int i = 0; i < count; i++)
        {
            bones[i] = new Entity(Native.aether_ragdoll_spawn_bone_at(i));
        }
        return bones;
    }

    /// <summary>
    /// Makes <paramref name="meshEntity"/>'s skin - its animator, its bone hierarchy,
    /// whatever <see cref="SkinnedMeshComponent"/> it carries - follow
    /// <paramref name="ragdollRoot"/>'s live physics pose every frame instead of its
    /// last sampled animation clip. The capsule/box bones spawned by
    /// <see cref="Spawn"/> keep rendering exactly as before; toggle
    /// <c>MeshRenderer.visible</c> on the bone entities (or on <paramref
    /// name="meshEntity"/>) to choose which one is actually seen. Safe to call before
    /// <paramref name="ragdollRoot"/> is (or after it stops being) a real ragdoll -
    /// nothing renders driven until it is.
    /// </summary>
    public static void DriveSkin(Entity meshEntity, Entity ragdollRoot) => Native.aether_ragdoll_drive_skin(meshEntity.Id, ragdollRoot.Id);
}
