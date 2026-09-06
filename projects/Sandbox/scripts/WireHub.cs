using System.Collections.Generic;
using System.Reflection;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The signal graph's one evaluator. Lives on a single scene entity named "WireHub" -
/// WireLink instances find it by that name (Scene.Find), not a static registry: a static
/// wire table would need cross-peer reconciliation once this ever goes networked, an
/// instance found by name does not (see PropSpawner's/PhysicsGun's own file comments on
/// the same reasoning for their own instance state).
///
/// DEVICE ROLES ARE A SMALL TABLE, NOT AN INTERFACE OR ATTRIBUTE. Button/Lever/Gate/
/// Lamp/Door/Emitter carry zero wire-specific code beyond plain public float fields
/// (Pressed/Out/A,B,Out/Enable) - this class is the only thing that knows those names,
/// via ordinary reflection (System.Reflection.FieldInfo), and the only thing that knows
/// which script type plays which role (see ResolveScript/RecomputeOutput). Adding
/// another device type is one more branch in each, nothing else - not a new interface
/// every device has to implement.
///
/// EVALUATION: Propagate() walks every registered wire in topological order (RebuildOrder,
/// Kahn's algorithm - cycles cannot exist because WouldCreateCycle refuses them at wire
/// creation, so this always terminates having placed every wire) and, for each wire,
/// recomputes the SOURCE's live output (RecomputeOutput - a no-op for Button/Lever/Lamp,
/// A/B->Out combinator for Gate) immediately before reading and propagating it. Recomputing
/// on demand, right before propagation, in dependency order, is what makes a straight
/// three-gate chain settle in a single Propagate() call instead of needing three frames -
/// it does not depend on Gate's own OnUpdate having already run this frame, which the
/// engine gives no ordering guarantee for (there is no OnLateUpdate here).
///
/// PUSH, NOT JUST POLL: OnUpdate calls Propagate() once a frame as a safety net (and to
/// prune dead-endpoint wires promptly), but Button also calls it directly, synchronously,
/// the instant it is pressed - eliminating the one remaining ordering question (does
/// WireHub's OnUpdate run before or after Button's own this frame?) entirely for the
/// common case, rather than accepting a possible one-frame lag on the graph's root event.
///
/// DEAD ENDPOINTS: a wire whose Source or Target entity was destroyed at runtime is
/// dropped from the graph the moment PruneDead notices (one Log.Warn, then removed for
/// good - never rechecked, so a later id-slot recycle can never resurrect it pointing at
/// something unrelated), and its own marker entity is destroyed with it. Never a crash,
/// never a silent retarget.
///
/// VISUAL: each WireLink owns a drawn segment between its own Source/Target (a
/// stretched cube - see WireLink's own file comment on why, and on the engine gap it
/// found: no persistent line/cylinder primitive exists, only a dev-only, off-by-default
/// debug-line gizmo). WireHub drives it at two different cadences on purpose: position
/// every frame (UpdateVisuals, below - a device can move on a frame no signal changes),
/// colour only when Propagate() actually reads a value (free at that point, since the
/// value is already computed for the write it is doing anyway).
/// </summary>
public sealed class WireHub : EntityScript
{
    private readonly List<WireLink> _links = new();
    private readonly HashSet<WireLink> _warnedDead = new();
    private List<WireLink> _order = new();
    private bool _orderDirty = true;

    public void Register(WireLink link)
    {
        if (_links.Contains(link))
        {
            return;
        }
        _links.Add(link);
        _orderDirty = true;
    }

    public void Unregister(WireLink link)
    {
        if (_links.Remove(link))
        {
            _orderDirty = true;
        }
    }

    /// <summary>Whether `target`'s `field` input already has a wire feeding it - lets
    /// ToolGun pick a Gate's first free input (A, then B) instead of always
    /// overwriting A.</summary>
    public bool IsInputConnected(Entity target, string field)
    {
        foreach (WireLink link in _links)
        {
            if (link.Target == target && link.InputField == field)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>Refuses a wire BEFORE it is created if it would close a cycle (a gate
    /// wired to its own input is the degenerate one-node case: source == target). Checked
    /// by ToolGun at the moment a link is proposed - a cycle that cannot be built
    /// cannot be discovered as a hang later, because RebuildOrder's topological sort has
    /// no way to represent one.</summary>
    public bool WouldCreateCycle(Entity source, Entity target)
    {
        if (source == target)
        {
            return true;
        }
        var visited = new HashSet<Entity>();
        var stack = new Stack<Entity>();
        stack.Push(target);
        while (stack.Count > 0)
        {
            Entity current = stack.Pop();
            if (current == source)
            {
                return true;
            }
            if (!visited.Add(current))
            {
                continue;
            }
            foreach (WireLink link in _links)
            {
                if (link.Source == current)
                {
                    stack.Push(link.Target);
                }
            }
        }
        return false;
    }

    /// <summary>Propagate() every frame, same as before; UpdateVisuals() is a separate
    /// pass at the same cadence - see its own comment for why it is not folded into
    /// Propagate() itself.</summary>
    public override void OnUpdate(float deltaTime)
    {
        Propagate();
        UpdateVisuals();
    }

    /// <summary>Evaluate the whole graph once, right now. Safe to call as often as
    /// needed - Button calls this directly on press so the entire downstream chain
    /// settles in that same call, not on whatever frame WireHub's own OnUpdate happens
    /// to run relative to Button's.</summary>
    public void Propagate()
    {
        PruneDead();
        if (_orderDirty)
        {
            RebuildOrder();
            _orderDirty = false;
        }

        foreach (WireLink link in _order)
        {
            object? source = ResolveScript(link.Source);
            object? target = ResolveScript(link.Target);
            if (source == null || target == null)
            {
                continue;
            }
            RecomputeOutput(source);
            float value = ReadFloat(source, link.OutputField);
            WriteFloat(target, link.InputField, value);
            // Free at this point - `value` is already computed for the write above,
            // this only spends the cost of applying it to a colour.
            link.SetVisualValue(value);
        }
    }

    /// <summary>Repositions every registered wire's visual segment from its Source/
    /// Target's CURRENT world positions - deliberately separate from Propagate()'s own
    /// call frequency (event-driven: Button.Interact() calls it directly, so a frame
    /// can see it run more than once, or not at all if nothing pressed anything) because
    /// a device can move - grabbed, a Door sliding - on a frame where no signal changes
    /// at all. Ten wires at one position read per endpoint plus one SetTransform write
    /// each is a few dozen calls a frame - negligible next to what physics/rendering
    /// already do every frame - so this runs unconditionally once a frame rather than
    /// only when a move is suspected.</summary>
    private void UpdateVisuals()
    {
        foreach (WireLink link in _links)
        {
            link.UpdateVisualTransform();
        }
    }

    private void PruneDead()
    {
        for (int i = _links.Count - 1; i >= 0; i--)
        {
            WireLink link = _links[i];
            // World.IsValid(entity), NOT entity.IsValid: the struct property is only
            // Id != 0 (correct for "did this raycast hit anything", wrong for "is this
            // SPECIFIC previously-known entity still alive") - confirmed live: deleting
            // a wired entity left link.Target.IsValid reading true forever, since the
            // stale Entity struct still held its old nonzero id. World.IsValid calls the
            // real registry check (aether_entity_valid) and is what actually flips false
            // once the id is gone.
            if (World.IsValid(link.Source) && World.IsValid(link.Target))
            {
                continue;
            }
            if (_warnedDead.Add(link))
            {
                Log.Warn($"[Sandbox] WireHub: dropping wire '{link.OutputField}' -> '{link.InputField}' - an endpoint was destroyed.");
            }
            _links.RemoveAt(i);
            _orderDirty = true;
            if (link.Self.IsValid)
            {
                link.Self.Destroy();
            }
        }
    }

    private void RebuildOrder()
    {
        var inDegree = new Dictionary<WireLink, int>();
        foreach (WireLink link in _links)
        {
            inDegree[link] = 0;
        }
        foreach (WireLink a in _links)
        {
            foreach (WireLink b in _links)
            {
                if (a != b && a.Target == b.Source)
                {
                    inDegree[b]++;
                }
            }
        }

        var ready = new Queue<WireLink>();
        foreach (WireLink link in _links)
        {
            if (inDegree[link] == 0)
            {
                ready.Enqueue(link);
            }
        }

        var order = new List<WireLink>();
        while (ready.Count > 0)
        {
            WireLink link = ready.Dequeue();
            order.Add(link);
            foreach (WireLink b in _links)
            {
                if (b != link && link.Target == b.Source && --inDegree[b] == 0)
                {
                    ready.Enqueue(b);
                }
            }
        }

        _order = order;
    }

    private static object? ResolveScript(Entity e)
    {
        if (!World.IsValid(e))
        {
            return null;
        }
        object? script = e.GetScript<Button>();
        if (script != null)
        {
            return script;
        }
        script = e.GetScript<Lever>();
        if (script != null)
        {
            return script;
        }
        script = e.GetScript<Gate>();
        if (script != null)
        {
            return script;
        }
        script = e.GetScript<Lamp>();
        if (script != null)
        {
            return script;
        }
        script = e.GetScript<Door>();
        if (script != null)
        {
            return script;
        }
        return e.GetScript<Emitter>();
    }

    private static void RecomputeOutput(object script)
    {
        if (script is Gate gate)
        {
            gate.Recompute();
        }
    }

    private static float ReadFloat(object script, string field)
    {
        FieldInfo? f = script.GetType().GetField(field);
        return f != null && f.FieldType == typeof(float) ? (float)f.GetValue(script)! : 0.0f;
    }

    private static void WriteFloat(object script, string field, float value)
    {
        FieldInfo? f = script.GetType().GetField(field);
        if (f != null && f.FieldType == typeof(float))
        {
            f.SetValue(script, value);
        }
    }
}
