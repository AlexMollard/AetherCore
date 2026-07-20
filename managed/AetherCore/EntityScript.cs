using System.Collections;
using System.Collections.Generic;

namespace AetherCore;

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

    // ── Physics callbacks (Unity-style) ───────────────────────────────────────────
    // Override any of these and the runtime auto-enables collision events on this
    // entity and dispatches them each frame - no polling needed.

    public virtual void OnCollisionEnter(Entity other) { }
    public virtual void OnCollisionExit(Entity other) { }
    public virtual void OnTriggerEnter(Entity other) { }
    public virtual void OnTriggerExit(Entity other) { }

    // 2D physics twins (Box2D-backed; fire in 2D scenes only).
    public virtual void OnCollisionEnter2D(Entity other) { }
    public virtual void OnCollisionExit2D(Entity other) { }
    public virtual void OnTriggerEnter2D(Entity other) { }
    public virtual void OnTriggerExit2D(Entity other) { }

    // 0 = undetermined, 1 = enabled + dispatching, 2 = no callbacks overridden.
    private int _physicsDispatch;
    private bool _dispatch3D;
    private bool _dispatch2D;

    private static bool Overrides(System.Type t, string method)
        => t.GetMethod(method)?.DeclaringType != typeof(EntityScript);

    internal void DispatchPhysicsEvents()
    {
        if (_physicsDispatch == 2)
        {
            return;
        }
        if (_physicsDispatch == 0)
        {
            System.Type t = GetType();
            _dispatch3D = Overrides(t, nameof(OnCollisionEnter)) || Overrides(t, nameof(OnCollisionExit))
                          || Overrides(t, nameof(OnTriggerEnter)) || Overrides(t, nameof(OnTriggerExit));
            _dispatch2D = Overrides(t, nameof(OnCollisionEnter2D)) || Overrides(t, nameof(OnCollisionExit2D))
                          || Overrides(t, nameof(OnTriggerEnter2D)) || Overrides(t, nameof(OnTriggerExit2D));
            if (!_dispatch3D && !_dispatch2D)
            {
                _physicsDispatch = 2;
                return;
            }
            if (_dispatch3D) { Physics.EnableEvents(Self); }
            if (_dispatch2D) { Physics2D.EnableEvents(Self); }
            _physicsDispatch = 1;
            return; // events start recording after enable; dispatch from next frame
        }
        if (_dispatch3D)
        {
            foreach (Entity e in Physics.GetCollisionEnter(Self)) { OnCollisionEnter(e); }
            foreach (Entity e in Physics.GetCollisionExit(Self)) { OnCollisionExit(e); }
            foreach (Entity e in Physics.GetTriggerEnter(Self)) { OnTriggerEnter(e); }
            foreach (Entity e in Physics.GetTriggerExit(Self)) { OnTriggerExit(e); }
        }
        if (_dispatch2D)
        {
            foreach (Entity e in Physics2D.GetCollisionEnter(Self)) { OnCollisionEnter2D(e); }
            foreach (Entity e in Physics2D.GetCollisionExit(Self)) { OnCollisionExit2D(e); }
            foreach (Entity e in Physics2D.GetTriggerEnter(Self)) { OnTriggerEnter2D(e); }
            foreach (Entity e in Physics2D.GetTriggerExit(Self)) { OnTriggerExit2D(e); }
        }
    }

    // ── Coroutines ────────────────────────────────────────────────────────────────

    private List<Coroutine>? _coroutines;

    /// <summary>Start a coroutine on this script (advances one step per frame,
    /// honouring WaitForSeconds / WaitForFrames). It runs to its first yield now.</summary>
    public Coroutine StartCoroutine(IEnumerator routine)
    {
        _coroutines ??= new List<Coroutine>();
        Coroutine co = new(routine);
        _coroutines.Add(co);
        co.Tick(0.0f); // run synchronously up to the first yield, like Unity
        return co;
    }

    public void StopCoroutine(Coroutine coroutine) => coroutine?.Stop();

    public void StopAllCoroutines()
    {
        if (_coroutines == null)
        {
            return;
        }
        foreach (Coroutine c in _coroutines)
        {
            c.Stop();
        }
        _coroutines.Clear();
    }

    // Called by the runtime once per frame BEFORE OnUpdate; advances running
    // coroutines and drops finished ones.
    internal void TickCoroutines(float deltaTime)
    {
        if (_coroutines == null || _coroutines.Count == 0)
        {
            return;
        }
        for (int i = _coroutines.Count - 1; i >= 0; i--)
        {
            if (!_coroutines[i].Tick(deltaTime))
            {
                _coroutines.RemoveAt(i);
            }
        }
    }
}
