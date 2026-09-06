using AetherCore;

namespace AetherGame;

/// <summary>
/// One small helper every runtime spawner in this project shares, instead of each
/// reinventing find-or-create - see WireHub's own file comment on why a "small table"
/// duplicated per caller drifts rather than staying in sync. A named container groups
/// everything a spawner creates under one Hierarchy panel entry instead of flooding the
/// scene root with loose objects (ten WireLink markers, a dozen spawned props, a growing
/// queue of Emitter balls, ...).
///
/// STATELESS ON PURPOSE - <see cref="Get"/> runs a live <see cref="Scene.Find"/> every
/// call rather than caching a result anywhere shared, so a container destroyed some
/// other way (or never created yet this session) can never be mistaken for one that
/// still exists - the same World.IsValid-vs-stale-handle trap <c>WireHub.PruneDead</c>'s
/// own file comment documents, just for a name lookup instead of an Entity handle. A
/// caller that spawns often and wants to skip the repeated lookup should cache the
/// RETURNED Entity itself, the same lazy-property idiom <c>Button.Hub</c> already uses
/// for <c>WireHub</c> - this method does not do that caching on anyone's behalf.
///
/// <paramref name="transient"/> IS NOT A STYLE CHOICE - IT DEPENDS ON WHAT THE CONTAINER
/// WILL HOLD, verified by reading the scene serializer rather than assumed:
/// <c>SceneSerializerCapture.cpp</c> skips an entity whenever
/// <c>ecs::HasSceneTransientAncestor</c> is true - checking the entity OR ANY ANCESTOR,
/// not just itself. A transient container therefore does not merely exclude ITSELF from
/// the save; it silently excludes every child parented under it too, transient or not.
/// That is exactly right for <c>Spawned Props</c>/<c>Emitted Balls</c> (their contents
/// are already individually <c>MarkTransient()</c>'d, so the ancestor check is a
/// harmless backstop) and exactly WRONG for <c>Wires</c>: <c>WireLink</c> deliberately
/// never calls <c>MarkTransient()</c> because a wire must survive save/load (see its own
/// file comment) - parenting it under a transient container would have silently undone
/// the exact persistence fix landed earlier today, by poisoning every wire's capture
/// eligibility through its new parent rather than through anything WireLink itself does.
/// <c>transient: false</c> builds the container the same way any other real, saved scene
/// entity is built (<see cref="World.Create"/> + <see cref="Entity.AddTransform"/> + a
/// name, no <see cref="Entity.MarkTransient"/>) so it round-trips through save/load like
/// any hand-authored grouping entity would, and <see cref="Scene.Find"/> picks the
/// reloaded one back up on the next wire exactly the way it already does for a
/// transient container.
///
/// NEVER MOVE THE RETURNED ENTITY - it is created at identity (world origin) and must
/// stay there for its entire life once it might have children. This is not just
/// tidiness: Entity.Position/EulerDegrees/Scale/SetTransform do NOT do ordinary
/// parent-local composition in this engine - they compute a WORLD delta from the
/// entity's old to new transform and re-apply that SAME delta to every child's current
/// world transform (TransformEdit.hpp's ApplyDeltaToSubtree, the cause of three separate
/// live bugs FirstPersonPlayer.cs's own header documents). Writing a container's
/// transform after it has children would silently drag every one of them along with it -
/// a spawner that teleports every prop it ever made the moment a second one spawns.
///
/// RE-PARENTING ITSELF IS SAFE, CONFIRMED BY READING THE NATIVE IMPLEMENTATION END TO
/// END, not assumed from how other engines behave: <c>aether_entity_set_parent</c>
/// (SceneExports.cpp) calls <c>aether::ecs::SetParent</c>, which is <c>InsertChildAt</c>
/// (Hierarchy.hpp) under the hood - it only ever reads and writes HierarchyComponent
/// (the parent field and both sides' children lists); it never reads or writes
/// TransformComponent at all. <c>Entity.SetParent</c> therefore never moves a child's
/// world position by so much as a float bit, in either direction, regardless of what
/// order spawning, positioning and parenting happen in.
/// </summary>
public static class RuntimeContainers
{
    /// <summary>The named container entity runtime-spawned things of one kind should
    /// live under - found by name if a session (or a previous save, for a persistent
    /// one) already has one, created fresh at identity otherwise. Never write the
    /// returned entity's own transform once it might have children - see this class's
    /// own file comment.</summary>
    /// <param name="transient">True (the common case) excludes both the container AND
    /// everything parented under it from the saved scene file - only correct when
    /// every child is itself meant to be transient. False makes the container a real,
    /// saved scene entity that a persistent child (a WireLink) can safely live under -
    /// see this class's own file comment for why the two are not interchangeable.</param>
    public static Entity Get(string name, bool transient = true)
    {
        Entity existing = Scene.Find(name);
        if (existing.IsValid)
        {
            return existing;
        }
        if (transient)
        {
            return Scene.Create(name);
        }
        Entity container = World.Create();
        container.Name = name;
        container.AddTransform();
        return container;
    }
}
