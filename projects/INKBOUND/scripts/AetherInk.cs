using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The signature mechanic: hold the left mouse button and move the cursor to paint a glowing ink
/// stroke wherever you point - no reach limit. The visual is a single continuous wet-ink layer
/// rendered entirely in a shader via the engine's generic CustomPass hook: this script registers a
/// pass running our project shader (assets/shaders/ink_field.slang) and submits the live stroke
/// segments to it each frame. Physics is separate and proper - each anchored segment gets one capsule
/// collider aligned to the segment, so a stroke reads as a smooth rounded ledge rather than a pile of
/// boxes.
///
/// Ink only holds where it can anchor to real geometry or a crystal; drawn in open air it turns to
/// red "ghost" ink - non-solid and crumbling - so you extend real ledges and bridge real gaps
/// rather than hovering. Drawing drains an aether meter (HUD) that refills only on actual ground or
/// from crystals, never on your own ink. Attach alongside PlayerController.
/// </summary>
public sealed class AetherInk : EntityScript
{
    // A full well draws ~9.4 world units of ink (75 / 4 segments at 0.5 spacing). Deliberately short
    // of the widest spans, so crossing one is a decision - go now, or top up at a crystal first -
    // rather than something a full meter solves without thinking. Refill rate and crystal value are
    // scaled to match, so the well still fills in about three seconds and a crystal is still worth
    // most of a tank.
    public float MaxAether = 75.0f;
    public float DrainPerSegment = 4.0f;
    public float RefillPerSecond = 25.5f;
    /// <summary>World distance between segment nodes as the stroke is laid.</summary>
    public float Spacing = 0.5f;
    /// <summary>Ink half-thickness in world units (drives both the shader width and the collider radius).</summary>
    public float Thickness = 0.11f;
    public float Lifetime = 5.0f;
    public float FadeTime = 1.6f;
    /// <summary>How close to real geometry a segment must be to count as TOUCHING it. Deliberately
    /// tight - ink has to meet the stone, not hover near it. Spans hold by staying joined to ink that
    /// is itself anchored (see LinkReach), not by being vaguely close to scenery.</summary>
    public float AnchorRadius = 0.4f;
    /// <summary>How far a segment may sit from already-anchored ink and still join onto it. A little
    /// over Spacing, so an unbroken stroke chains, but a fresh stroke in open air does not.</summary>
    public float LinkReach = 0.75f;
    /// <summary>Crystals are deliberate anchor points, so they hold ink from a bit further off.</summary>
    public float CrystalReach = 1.1f;

    // Read by HudController for the meter bar.
    public static float Aether;
    public static float AetherMax = 100.0f;

    // The stroke draws emissive (after the light map), so these are its final on-screen colours: the
    // body stays dark ink, the rim carries the glow that reads against a black cave.
    private static readonly Vector4 BodyColor = new(0.05f, 0.10f, 0.14f, 1.0f); // dark ink body
    private static readonly Vector4 RimColor = new(0.40f, 0.88f, 1.00f, 1.0f);  // ink-cyan glow

    // Lighting cast by the stroke itself (see the pack loop). One light every LightStride segments
    // keeps the per-pixel light loop cheap while still reading as a continuous glow.
    private const int LightStride = 4;
    private const float InkLightRadius = 2.6f;
    private const float InkLightIntensity = 1.7f;
    private static readonly Vector3 InkLightColor = new(0.35f, 0.85f, 1.0f);   // ink-cyan
    private static readonly Vector3 GhostLightColor = new(1.0f, 0.30f, 0.26f); // ghost ink = red

    // The engine's generic project-pass hook renders the ink; this project owns the shader + packing.
    private const string PassName = "inkfield";
    // Reused scratch buffer: two Vector4 per segment (a.xy,b.xy | width,alpha,glow,ghost).
    private readonly List<Vector4> _buf = new();

    private struct Seg
    {
        public Vector2 A, B;
        public float Age;
        public bool Anchored;
        public Entity Collider; // invalid for ghost ink
    }

    private readonly List<Seg> _segs = new();

    /// <summary>Ink that has SET. When the wanderer comes apart, whatever they were holding up stops
    /// being conjured and becomes part of the cave - the ledge you died on is still there for the next
    /// attempt. Petrified ink never ages, and it is real ground: you can anchor to it and stand on it.
    /// It is also a bank, not a monument - hold the right mouse button over it to draw it back into
    /// the well, so nothing you fossilise can wall the level off for good.</summary>
    private readonly List<Seg> _petrified = new();

    /// <summary>How close the cursor must be to petrified ink to reclaim it.</summary>
    public float ReclaimRadius = 0.9f;
    /// <summary>Aether returned per segment reclaimed. Slightly under what it cost, so shuffling ink
    /// around the cave is a real decision and not a free undo.</summary>
    public float ReclaimRefund = 3.0f;
    /// <summary>Aether returned for unmaking a creature - killing feeds the well.</summary>
    public float SmotherRefund = 8.0f;

    // Live ink-collider ids, so anchoring and "is the player on real ground?" checks can tell
    // conjured ink from actual terrain.
    private static readonly HashSet<uint> s_inkColliders = new();
    public static bool IsInk(uint id) => s_inkColliders.Contains(id);

    // Petrified ink IS terrain as far as the game is concerned, so it is deliberately NOT in the
    // ink set - the player counts as standing on real ground when they stand on it.
    private static readonly HashSet<uint> s_petrifiedColliders = new();
    public static bool IsPetrified(uint id) => s_petrifiedColliders.Contains(id);

    private Vector2 _last;
    private bool _hasLast;
    private float _t;
    private float _dripCd; // throttles ink-droplet spawns while drawing

    /// <summary>The live ink for this run, so the player can hand it their death.</summary>
    public static AetherInk? Instance;

    public override void OnAttach()
    {
        Instance = this;
        Aether = MaxAether;
        AetherMax = MaxAether;
        _segs.Clear();
        _petrified.Clear();
        s_inkColliders.Clear(); // stale ids from a previous level never carry over
        s_petrifiedColliders.Clear();
        _hasLast = false;
        // Register the ink field as a project custom pass: the engine runs our ink_field shader over
        // the scene each frame we submit segments to it.
        // Emissive: the ink glows, so it draws after the 2D light map and keeps its own brightness.
        // It still lights and shadows the cave through the Lighting2D submissions in OnUpdate.
        CustomPass.Register(PassName, "ink_field", CustomPassStage.EmissiveOverLight);
    }

    public override void OnDetach()
    {
        // Leaving the level: stop drawing the pass so no stray ink renders over the next scene.
        CustomPass.Unregister(PassName);
        s_inkColliders.Clear();
        s_petrifiedColliders.Clear();
        if (Instance == this) { Instance = null; }
    }

    /// <summary>The wanderer comes apart: every span they were holding up SETS. Called by
    /// PlayerController on death, however that death came about. The colliders are kept exactly as
    /// they are - what was a conjured ledge a moment ago is now simply part of the cave.</summary>
    public void Petrify()
    {
        int set = 0;
        for (int i = 0; i < _segs.Count; i++)
        {
            Seg s = _segs[i];
            if (!s.Anchored || !s.Collider.IsValid) { continue; }
            s_inkColliders.Remove(s.Collider.Id);
            s_petrifiedColliders.Add(s.Collider.Id);
            s.Age = 0.0f;
            _petrified.Add(s);
            set++;
        }
        // Ghost ink was never real, so it simply goes with the body.
        for (int i = 0; i < _segs.Count; i++)
        {
            Seg s = _segs[i];
            if (s.Anchored) { continue; }
            if (s.Collider.IsValid) { s.Collider.Destroy(); }
        }
        _segs.Clear();
        _hasLast = false;
        if (set > 0)
        {
            Log.Info($"[INKBOUND] {set} span(s) set into the cave.");
        }
    }

    /// <summary>Draw petrified ink back into the well: hold the right mouse button over it. This is
    /// what keeps fossilised ink from ever becoming a wall you cannot undo.</summary>
    private void Reclaim(Vector2 at)
    {
        float r2 = ReclaimRadius * ReclaimRadius;
        for (int i = _petrified.Count - 1; i >= 0; i--)
        {
            Seg s = _petrified[i];
            Vector2 mid = (s.A + s.B) * 0.5f;
            if (Vector2.DistanceSquared(at, mid) > r2) { continue; }
            if (s.Collider.IsValid)
            {
                s_petrifiedColliders.Remove(s.Collider.Id);
                s.Collider.Destroy();
            }
            _petrified.RemoveAt(i);
            Aether = Math.Min(MaxAether, Aether + ReclaimRefund);
            Scene.Instantiate("InkDroplet", new Vector3(mid.X, mid.Y, 0.0f));
            return; // one segment per frame, so reclaiming reads as a steady drain rather than a pop
        }
    }

    /// <summary>Ink is the weapon. Anything smotherable under a freshly laid span is unmade, and its
    /// substance goes to the well - so a kill is also a refill.</summary>
    private void SmotherUnder(Vector2 a, Vector2 b)
    {
        Vector2 mid = (a + b) * 0.5f;
        foreach (Entity e in Physics2D.OverlapCircle(mid, Thickness + 0.45f))
        {
            if (e.Id == Self.Id || IsInk(e.Id) || IsPetrified(e.Id)) { continue; }
            if (Creature.TrySmother(e.Id))
            {
                Aether = Math.Min(MaxAether, Aether + SmotherRefund);
            }
        }
    }

    public override void OnUpdate(float deltaTime)
    {
        AetherMax = MaxAether;
        _t += deltaTime;
        _dripCd -= deltaTime;

        // 1. Drawing input -> lay evenly spaced segments along the cursor's path.
        if (!GameState.Won)
        {
            if (Input.IsMousePressed(MouseButton.Left))
            {
                _hasLast = false;
            }

            // Right button takes matter back out of the cave: hold it over set ink to drink it in.
            if (Input.IsMouseDown(MouseButton.Right) && _petrified.Count > 0)
            {
                Vector3 c = Camera.ScreenToWorld(Input.MousePosition);
                Reclaim(new Vector2(c.X, c.Y));
            }

            bool drawing = Input.IsMouseDown(MouseButton.Left) && Aether > 0.0f;
            if (drawing)
            {
                Vector3 cursor = Camera.ScreenToWorld(Input.MousePosition);
                Vector2 point = new(cursor.X, cursor.Y);
                if (!_hasLast)
                {
                    _last = point;
                    _hasLast = true;
                }
                else
                {
                    // Walk the gap in fixed steps so a fast flick still lays a continuous stroke.
                    float gap = Vector2.Distance(point, _last);
                    while (gap >= Spacing && Aether > 0.0f)
                    {
                        Vector2 next = _last + Vector2.Normalize(point - _last) * Spacing;
                        if (LaySegment(_last, next)) // no aether spent on ink that can't be placed (inside a wall)
                        {
                            Aether = Math.Max(0.0f, Aether - DrainPerSegment);
                        }
                        _last = next;
                        gap = Vector2.Distance(point, _last);
                    }
                }

                // Drip: throttled ink droplets fall off the freshly drawn point and splat on the ground.
                if (_dripCd <= 0.0f)
                {
                    Scene.Instantiate("InkDroplet", new Vector3(_last.X, _last.Y - 0.1f, 0.0f));
                    _dripCd = 0.11f + 0.08f * (0.5f + 0.5f * MathF.Sin(_t * 27.3f)); // ~0.11-0.19s, jittered
                }
            }
            else if (IsOnRealGround())
            {
                // Refill only on real terrain (crystals handled by AetherCrystal) - never on your own ink.
                Aether = Math.Min(MaxAether, Aether + RefillPerSecond * deltaTime);
            }
        }

        // 2. Age segments, expire the dead (and their colliders), and pack the live ones into the
        //    scratch buffer. Rebuilt every frame so the shader always has the current stroke. Track the
        //    world AABB so the pass quad only covers the ink (not the whole screen).
        _buf.Clear();
        float minX = float.MaxValue, minY = float.MaxValue, maxX = float.MinValue, maxY = float.MinValue;
        for (int i = _segs.Count - 1; i >= 0; i--)
        {
            Seg s = _segs[i];
            s.Age += deltaTime;
            float life = s.Anchored ? Lifetime : 0.55f;
            float fade = s.Anchored ? FadeTime : 0.4f;

            if (s.Age >= life)
            {
                if (s.Collider.IsValid)
                {
                    s_inkColliders.Remove(s.Collider.Id);
                    s.Collider.Destroy();
                }
                _segs.RemoveAt(i);
                continue;
            }

            float alpha = s.Age > life - fade ? Math.Clamp((life - s.Age) / fade, 0.0f, 1.0f) : 1.0f;
            float grow = Math.Clamp(s.Age / 0.08f, 0.4f, 1.0f); // quick pop-in
            float glow = s.Anchored ? 0.4f + 0.6f * GameSettings.InkGlow : 0.5f; // Ink Glow scales the rim
            float ghost = s.Anchored ? 0.0f : 1.0f;                              // ghost = red
            _buf.Add(new Vector4(s.A.X, s.A.Y, s.B.X, s.B.Y));
            _buf.Add(new Vector4(Thickness * grow, s.Anchored ? alpha : alpha * 0.85f, glow, ghost));

            // The stroke lights the cave and blocks light. Anchored ink is real matter, so it casts a
            // shadow capsule matching its drawn thickness; ghost ink is crumbling and non-solid, so it
            // only glows. Lights are sampled every LightStride segments - one per segment would flood
            // the per-pixel light loop - which still reads as a continuous glowing line.
            if (s.Anchored)
            {
                Lighting2D.SubmitOccluder(s.A, s.B, Thickness * grow);
            }
            if (i % LightStride == 0)
            {
                Vector2 mid = (s.A + s.B) * 0.5f;
                Vector3 tint = s.Anchored ? InkLightColor : GhostLightColor;
                Lighting2D.SubmitLight(mid, InkLightRadius, tint, InkLightIntensity * alpha * glow);
            }
            minX = Math.Min(minX, Math.Min(s.A.X, s.B.X));
            minY = Math.Min(minY, Math.Min(s.A.Y, s.B.Y));
            maxX = Math.Max(maxX, Math.Max(s.A.X, s.B.X));
            maxY = Math.Max(maxY, Math.Max(s.A.Y, s.B.Y));
            _segs[i] = s;
        }

        // Set ink draws in the same pass, flagged ghost = -1 so the shader renders it as dull rock
        // instead of glowing ink. It never ages, so it is packed straight through at full alpha.
        for (int i = 0; i < _petrified.Count; i++)
        {
            Seg s = _petrified[i];
            _buf.Add(new Vector4(s.A.X, s.A.Y, s.B.X, s.B.Y));
            _buf.Add(new Vector4(Thickness * 1.15f, 1.0f, 0.0f, -1.0f));
            minX = Math.Min(minX, Math.Min(s.A.X, s.B.X));
            minY = Math.Min(minY, Math.Min(s.A.Y, s.B.Y));
            maxX = Math.Max(maxX, Math.Max(s.A.X, s.B.X));
            maxY = Math.Max(maxY, Math.Max(s.A.Y, s.B.Y));
        }

        // params = world AABB of the ink (min.xy, max.xy), padded for thickness + rim + edge noise, so
        // the shader's fullscreen pass only rasterises pixels near the ink. Zero = degenerate = no draw.
        Vector4 aabb = Vector4.Zero;
        if (_buf.Count > 0)
        {
            const float pad = 0.7f; // covers thickness + rim + feathered bleed edge
            aabb = new Vector4(minX - pad, minY - pad, maxX + pad, maxY + pad);
        }

        // Submit the whole stroke to the ink pass (body + rim colours as color0/color1). Even an
        // empty buffer submits so the pass stays registered; the shader just draws nothing.
        CustomPass.Submit(PassName, CollectionsMarshal.AsSpan(_buf), aabb, BodyColor, RimColor);
    }

    /// <summary>Lay one segment. Returns false (and lays nothing) if it would sit inside solid
    /// geometry - you can't draw ink through walls or into objects.</summary>
    private bool LaySegment(Vector2 a, Vector2 b)
    {
        Vector2 mid = (a + b) * 0.5f;
        if (IsInsideSolid(mid))
        {
            return false;
        }

        bool anchored = EvaluateAnchor(mid);
        Seg s = new() { A = a, B = b, Age = 0.0f, Anchored = anchored };

        if (anchored)
        {
            // Proper collider: one capsule aligned to the segment (local Y is the capsule's long
            // axis, so rotate the entity by the segment angle minus 90 degrees).
            Vector2 d = b - a;
            float len = d.Length();
            float angleDeg = MathF.Atan2(d.Y, d.X) * (180.0f / MathF.PI);
            float height = MathF.Max(len, Thickness * 2.0f + 0.02f);

            Entity e = Scene.Create("InkSeg", new Vector3(mid.X, mid.Y, 0.0f));
            e.SetTransform(new Vector3(mid.X, mid.Y, 0.0f), new Vector3(0.0f, 0.0f, angleDeg - 90.0f), Vector3.One);
            Physics2D.AddCapsuleBody(e, Thickness, height, Body2DType.Static);
            s.Collider = e;
            s_inkColliders.Add(e.Id);
        }

        _segs.Add(s);
        SmotherUnder(a, b); // drawing across a creature drowns it
        return true;
    }

    /// <summary>Does this point sit inside solid terrain (a wall/ground tile)? Uses the tilemap tile
    /// solidity so it's correct deep inside a block - the ground's chain colliders are hollow, so an
    /// overlap query only caught the surface edge and let ink be drawn inside. Points in empty cells
    /// (including just above a surface) return false, so you can still draw ledges onto walls.</summary>
    private static bool IsInsideSolid(Vector2 p) => Physics2D.IsPointSolid(p);

    /// <summary>Is real geometry (or a crystal) within reach of this point? Other ink does not count
    /// - a span must ultimately reach the actual world to hold.
    ///
    /// Anchoring reads off the TILEMAP, not off a physics overlap. An overlap query counted every
    /// collider in range, so a floating coin, a dialogue zone or a chomper wandering past silently
    /// anchored ink in mid-air - the rule looked random because its real anchors were invisible.
    /// Terrain is the only thing that holds ink, plus crystals, which advertise themselves.</summary>
    private bool EvaluateAnchor(Vector2 p)
    {
        // 1. Touching real stone holds outright.
        if (TouchesTerrain(p)) { return true; }

        // 2. Joined to ink that is itself anchored. This is what lets a span cross a gap: you start
        //    the stroke on the ledge and every following segment holds onto the one before it, all the
        //    way back to the rock. It also makes reach honest - how far you get depends on your line
        //    staying connected, not on how much scenery happens to sit within some radius.
        float link = LinkReach * LinkReach;
        for (int i = _segs.Count - 1; i >= 0; i--)
        {
            Seg s = _segs[i];
            if (!s.Anchored) { continue; }
            if (Vector2.DistanceSquared(p, s.B) <= link || Vector2.DistanceSquared(p, s.A) <= link)
            {
                return true;
            }
        }

        // 3. Crystals are deliberate anchor points out in a chasm, so they reach a little further.
        foreach (Entity e in Physics2D.OverlapCircle(p, CrystalReach))
        {
            if (AetherCrystal.IsCrystal(e.Id)) { return true; }
        }
        return false;
    }

    /// <summary>Is this point actually against solid terrain? Samples the tilemap in a tight disc,
    /// indexed off zero so the straight-down/left/right/up samples are always taken.</summary>
    private bool TouchesTerrain(Vector2 p)
    {
        const float Step = 0.2f;
        int n = (int)MathF.Ceiling(AnchorRadius / Step);
        for (int iy = -n; iy <= n; iy++)
        {
            for (int ix = -n; ix <= n; ix++)
            {
                float dx = ix * Step, dy = iy * Step;
                if (dx * dx + dy * dy > AnchorRadius * AnchorRadius) { continue; }
                if (Physics2D.IsPointSolid(new Vector2(p.X + dx, p.Y + dy))) { return true; }
            }
        }
        return false;
    }

    /// <summary>Grounded on ACTUAL terrain, not on conjured ink.</summary>
    private bool IsOnRealGround()
    {
        Vector3 pos = Self.Position;
        for (float offset = -0.28f; offset <= 0.28f; offset += 0.56f)
        {
            RaycastHit2D hit = Physics2D.Raycast(new Vector2(pos.X + offset, pos.Y), new Vector2(0.0f, -1.0f), 0.85f);
            if (hit.DidHit && !IsInk(hit.Entity.Id))
            {
                return true;
            }
        }
        return false;
    }
}
