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
    ///
    /// <see cref="HideInInspectorAttribute"/>: this has exactly one correct value,
    /// the entity <see cref="Bind"/> was constructed with, and never a second
    /// legitimate one - unlike a genuine cross-entity reference (e.g.
    /// WireLink.Source/Target), which must stay author-editable and persisted.
    /// Reflecting it turned every entity insertion/deletion/reorder into a
    /// potential silent mis-wire: the array-position remap that scene refs used
    /// to go through could point a script's own Self field at a different entity
    /// after a save/load, and a live instance would keep running with the wrong
    /// one until destroyed. Excluding it from BuildProps removes it from the
    /// Inspector, from scene capture, and from ApplyProperties entirely, so a
    /// stale or wrong persisted Self value can never reach a live instance again -
    /// Self is authoritative-at-construction, full stop.
    /// </summary>
    [HideInInspector]
    public Entity Self;

    internal void Bind(Entity self) => Self = self;

    /// <summary>
    /// Another script of type <typeparamref name="T"/> on this same entity, or
    /// <c>null</c> if there is none - shorthand for <c>Self.GetScript&lt;T&gt;()</c>,
    /// which is where the full behaviour (duplicate types, attach ordering) is
    /// documented. Returns null rather than throwing, like Unity's GetComponent.
    /// </summary>
    public T? GetScript<T>() where T : EntityScript => ScriptInstances.Find<T>(Self.Id);

    public virtual void OnAttach() { }

    public virtual void OnUpdate(float deltaTime) { }

    public virtual void OnDetach() { }

    /// <summary>
    /// Fires once ownership of <see cref="Self"/> becomes knowable, and again every
    /// time it changes hands afterward - the one place to do owner-dependent setup
    /// that <see cref="OnAttach"/> cannot.
    /// </summary>
    /// <remarks>
    /// On a client, <see cref="Net.IsOwner"/> answers false for EVERYTHING until the
    /// host's Welcome lands, so ownership is not knowable at attach time - and games
    /// used to work around that by re-deriving owner-dependent state every frame in
    /// <see cref="OnUpdate"/> "just in case it just became true", each occurrence
    /// carrying its own paragraph explaining why. This hook is that moment instead:
    /// the runtime calls it exactly when the answer is decided, so owner-dependent
    /// setup runs once, here, rather than being polled forever.
    /// <para>
    /// Fires on the SAME frame as <see cref="OnAttach"/>, right after it, when the
    /// entity is not networked at all, when this process is offline, and when this
    /// process is the host - all three know their ownership from the very first
    /// frame. On a client behind a replicated <c>NetworkIdentity</c> it fires the
    /// first frame the host's Welcome has landed, and again if the owning connection
    /// ever changes (e.g. that connection disconnecting hands the entity to the
    /// host) - never per frame otherwise. A script does not need to know which of
    /// these cases it is in.
    /// </para>
    /// </remarks>
    /// <param name="owner">The connection id that owns <see cref="Self"/> (the host's
    /// id for a host-owned or unreplicated entity).</param>
    /// <param name="isOwner">The same answer <see cref="Net.IsOwner"/> would give right
    /// now, delivered once instead of polled.</param>
    public virtual void OnOwnershipChanged(uint owner, bool isOwner) { }

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
            // A coroutine body can mutate the list while we are inside Tick
            // (StopAllCoroutines clears it, StartCoroutine appends), so the index
            // must be re-checked against the live count on every step.
            if (i >= _coroutines.Count)
            {
                break;
            }
            Coroutine co = _coroutines[i];
            if (!co.Tick(deltaTime))
            {
                // Remove by identity, not index: the body may have cleared the
                // list while the routine was finishing, making i stale.
                _coroutines.Remove(co);
            }
        }
    }
}
