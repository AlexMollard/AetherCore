namespace AetherCore.Managed;

/// <summary>
/// Base class for gameplay scripts attached to entities via a ScriptComponent.
/// One instance is created per entity, so instance fields are naturally
/// per-entity state (no cross-entity sharing of behavior state).
///
/// The runner calls <see cref="OnAttach"/> the first tick the entity is seen
/// while playing, <see cref="OnUpdate"/> every tick after, and
/// <see cref="OnDetach"/> when the entity or component goes away (or on reload).
/// </summary>
public abstract class EntityScript
{
    /// <summary>
    /// The entity this script instance drives. Assigned by the runtime before
    /// OnAttach. A field (not a property) so scripts can write through it, e.g.
    /// <c>Self.Position = ...</c>.
    /// </summary>
    public Entity Self;

    internal void Bind(Entity self) => Self = self;

    public virtual void OnAttach() { }

    public virtual void OnUpdate(float deltaTime) { }

    public virtual void OnDetach() { }
}
