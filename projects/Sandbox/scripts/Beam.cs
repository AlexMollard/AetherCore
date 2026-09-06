using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A player-visible energy beam - a chain of segments following a curve, not one
/// straight stretched cube. Shared machinery: PhysicsGun's grab laser and any future
/// ToolGun mode that needs a beam (Rope, once physics-engine's distance constraint
/// lands) both drive an instance of this instead of each rolling their own, the same
/// "small table, not duplicated per caller" reasoning WireHub's own file comment gives
/// for device roles.
///
/// LADDER CHECKED FIRST: grepped src/engine/rendering/ for a line/trail/beam primitive
/// that might have landed since WireLink last checked - nothing new. Debug.DrawLine
/// remains the only line-drawing capability and is still explicitly a dev-only, one-frame
/// gizmo (its own doc comment), unusable for a player-visible effect. A stretched cube
/// (World.CreateMesh("cube")) is still the shape that exists, so N of them, chained, is
/// what this uses - exactly WireLink's own reasoning, multiplied.
///
/// NOT AN EntityScript. A beam is driven every frame by whichever script owns it
/// (PhysicsGun.OnUpdate, a future ToolGun mode) via a synchronous <see cref="Show"/>
/// call, the same way WireLink owns and drives its own single visual child entity
/// directly - a helper does not need to be its own attachable script merely because it
/// manages entities.
///
/// CURVE, NOT A STRAIGHT LINE: <see cref="SegmentCount"/> points are sampled along the
/// straight line from `start` to a SMOOTHED end position, each displaced sideways by a
/// sine wave that is zero at both endpoints (so the beam still visibly touches its start
/// and end) and largest at the midpoint - "envelope times oscillation", the same shape a
/// whip or rope reads as without simulating one.
///
/// LAG, NOT A SNAP: the rendered end position is exponentially damped toward the real
/// target every frame (<see cref="FollowLag"/>) rather than assigned directly - a fast
/// aim swing visibly drags the beam's tip behind it for a few frames before it catches
/// up, which is most of what makes this read as a beam with some give rather than a
/// rigid laser pointer. Frame-rate independent (1 - exp(-rate * dt)), not a fixed
/// per-frame fraction - the same reasoning PhysicsGun's own DriveHeldProp spring gives
/// for not snapping a held prop straight to its target.
///
/// SEGMENT COUNT: 10, the middle of the instructed 8-16 range. Below ~8 the sine
/// displacement reads as a jagged polyline instead of a curve; above ~16 the extra
/// segments buy no visible smoothness (the amplitude is a few tens of centimetres at
/// most) for a proportional rise in per-frame SetTransform calls. Cost per visible beam
/// is 10 SetTransform calls a frame, only while it is shown - the same per-instance cost
/// WireLink already pays once per wire, just multiplied by a small constant, and this
/// project rarely has more than one or two beams live at once (one per gun that is
/// actively firing, not one per player-owned entity). If a future use case wants many
/// simultaneous beams, that is the number to revisit first.
///
/// TRANSIENT, DELIBERATELY THE OPPOSITE OF "Wires": a beam is a firing/aiming effect,
/// never meant to survive a save - MarkTransient() on the container AND every segment,
/// parented under its own <c>RuntimeContainers.Get("Beams", transient: true)</c>, never
/// under "Wires". RuntimeContainers' own file comment documents why the two containers
/// cannot be interchanged: "Wires" is built non-transient specifically so a persistent
/// WireLink can live under it - parenting a transient beam there would not corrupt
/// anything the beam itself needs (it was never going to be saved either way) but would
/// pollute "Wires" as a Hierarchy-panel grouping of only wires. Separate container,
/// always.
///
/// Entity.Destroy does NOT cascade to children (confirmed by reading
/// ScriptComponentSystem.cpp's pending-destroy flush - see WireLink's own file comment on
/// the same fact), so <see cref="Destroy"/> below destroys every segment explicitly
/// before the container, exactly like WireLink.OnDetach does for its own single visual.
///
/// PUBLISHED API for PhysicsGun (laser/grab) and ToolGun (future Rope):
/// <see cref="Create"/> once per beam a script wants to own; <see cref="Show"/> every
/// frame it should be visible, from the muzzle to either a fixed point (a raycast hit) or
/// a live entity (a grabbed prop, via the <see cref="Show(Vector3,Entity,float)"/>
/// overload, which re-reads the target's current position every call so it tracks a
/// moving prop with no extra wiring); <see cref="Hide"/> when it should not be; and
/// <see cref="Destroy"/> once, from the owning script's OnDetach.
/// </summary>
public sealed class Beam
{
    /// <summary>Emissive-bright by default so it reads as energy, not a grey prop -
    /// set before or between <see cref="Show"/> calls to recolour.</summary>
    public Vector3 Color = new(0.25f, 0.85f, 1.0f);

    public float Thickness = 0.03f;

    /// <summary>Sideways displacement at the curve's midpoint, in world units.</summary>
    public float WaveAmplitude = 0.06f;

    /// <summary>How fast the wave's phase advances, in radians per second.</summary>
    public float WaveFrequency = 8.0f;

    /// <summary>How fast the rendered end position catches up to the real one. Higher is
    /// snappier and closer to a straight-line follow; lower drags more. 12 settles a
    /// sudden foot-scale aim swing in a few frames without looking like it never catches
    /// up at all.</summary>
    public float FollowLag = 12.0f;

    private const int SegmentCount = 10;

    private readonly Entity _container;
    private readonly Entity[] _segments;
    private Vector3 _smoothedEnd;
    private Vector3 _appliedColor = new(-1.0f, -1.0f, -1.0f);
    private float _time;
    private bool _hasEnd;
    private bool _visible;

    private Beam(Entity container, Entity[] segments)
    {
        _container = container;
        _segments = segments;
    }

    /// <summary>Builds the beam's container and every segment up front, all hidden
    /// (SetActive(false)) until the first <see cref="Show"/> call - the same "renders
    /// nothing until endpoints resolve" convention WireLink's own EnsureVisual uses,
    /// just via visibility instead of zero scale, since these segments are recomputed
    /// and reactivated every frame they are shown anyway.</summary>
    public static Beam Create(string name = "Beam")
    {
        Entity container = World.Create();
        container.Name = name;
        container.AddTransform();
        container.MarkTransient();
        container.SetParent(RuntimeContainers.Get("Beams", transient: true));

        var segments = new Entity[SegmentCount];
        for (int i = 0; i < SegmentCount; i++)
        {
            Entity segment = World.Create();
            segment.Name = $"{name} Segment {i}";
            segment.AddTransform();
            segment.AddMesh(World.CreateMesh("cube"));
            segment.MarkTransient();
            segment.SetParent(container);
            segment.SetActive(false);
            segments[i] = segment;
        }
        return new Beam(container, segments);
    }

    /// <summary>Show/update the beam for this frame, from `start` to `end`. Call every
    /// frame the beam should be visible - there is no separate "is showing" toggle to
    /// remember to flip; skipping a frame's call is exactly equivalent to
    /// <see cref="Hide"/> having been called, and the next call after a gap resumes from
    /// wherever the lagged end last was rather than snapping back first.</summary>
    public void Show(Vector3 start, Vector3 end, float deltaTime)
    {
        _smoothedEnd = _hasEnd ? Vector3.Lerp(_smoothedEnd, end, 1.0f - MathF.Exp(-FollowLag * deltaTime)) : end;
        _hasEnd = true;
        _time += deltaTime;

        Vector3 delta = _smoothedEnd - start;
        float length = delta.Length();
        if (length < 0.001f)
        {
            Hide();
            return;
        }
        Vector3 direction = delta / length;
        // An arbitrary perpendicular to displace along - Vector3.UnitY unless the beam
        // is nearly vertical, in which case that cross product degenerates toward zero
        // and UnitX is used instead.
        Vector3 reference = MathF.Abs(direction.Y) > 0.99f ? Vector3.UnitX : Vector3.UnitY;
        Vector3 side = Vector3.Normalize(Vector3.Cross(direction, reference));

        if (Color != _appliedColor)
        {
            foreach (Entity segment in _segments)
            {
                segment.SetMaterialColor(Color);
            }
            _appliedColor = Color;
        }

        Vector3 previous = start;
        for (int i = 0; i < SegmentCount; i++)
        {
            float t = (float)(i + 1) / SegmentCount;
            Vector3 point = PointOnCurve(start, side, length, direction, t);
            _segments[i].SetActive(true);
            SegmentVisual.Orient(_segments[i], previous, point, Thickness);
            previous = point;
        }
        _visible = true;
    }

    /// <summary>Convenience overload for a beam that should follow a live entity (a
    /// grabbed prop) rather than a fixed point - re-reads `endTarget.Position` every
    /// call, so the caller does not have to track it separately. Hides and returns if
    /// `endTarget` has been destroyed, checked with World.IsValid - the same
    /// stale-handle-safe check WireHub.PruneDead uses, not the always-true
    /// Entity.IsValid a stale handle would still pass.</summary>
    public void Show(Vector3 start, Entity endTarget, float deltaTime)
    {
        if (!World.IsValid(endTarget))
        {
            Hide();
            return;
        }
        Show(start, endTarget.Position, deltaTime);
    }

    private Vector3 PointOnCurve(Vector3 start, Vector3 side, float length, Vector3 direction, float t)
    {
        Vector3 onLine = start + direction * (length * t);
        float envelope = MathF.Sin(t * MathF.PI);
        float wave = MathF.Sin(t * MathF.PI * 2.0f + _time * WaveFrequency) * WaveAmplitude * envelope;
        return onLine + side * wave;
    }

    /// <summary>Deactivates every segment without destroying them, so a beam that fires
    /// intermittently (aim, release, aim again) is cheap to bring back - no
    /// re-allocation, just Entity.SetActive flips.</summary>
    public void Hide()
    {
        if (!_visible)
        {
            return;
        }
        foreach (Entity segment in _segments)
        {
            segment.SetActive(false);
        }
        _visible = false;
        _hasEnd = false;
    }

    /// <summary>Tears the whole beam down - every segment explicitly, then the
    /// container, since Entity.Destroy does not cascade (see this class's own file
    /// comment). Call once from the owning script's OnDetach.</summary>
    public void Destroy()
    {
        foreach (Entity segment in _segments)
        {
            if (World.IsValid(segment))
            {
                segment.Destroy();
            }
        }
        if (World.IsValid(_container))
        {
            _container.Destroy();
        }
    }
}
