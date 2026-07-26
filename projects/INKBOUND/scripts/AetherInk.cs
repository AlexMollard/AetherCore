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
    // Slow. The well used to refill in about three seconds, so waiting was always free and the budget
    // never actually bit - you could solve anything by standing still first. Now a full well is the
    // best part of ten seconds, which makes crystals worth crossing a room for and makes spending
    // twenty of it on a creature something you feel.
    public float RefillPerSecond = 8.5f;
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
    /// <summary>How far the guide looks for stone when the cursor will not hold. Generous on purpose:
    /// the failure that feels like a bug is the NEAR miss, so the range has to comfortably cover
    /// "I thought I was touching that wall". Past it there is nothing to have nearly hit, and the guide
    /// says "no" without pointing anywhere.</summary>
    public float HintReach = 2.2f;

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
    /// <summary>What it COSTS to unmake a creature. Smothering used to refund more ink than the
    /// stroke spent, which made killing everything strictly better than avoiding anything - there was
    /// never a reason to be careful. Now drowning something is the most expensive thing you can do
    /// with your substance, so the real question at every creature is "pay, or find a way past".
    /// Too poor to pay and the ink simply washes over it, still alive.</summary>
    public float SmotherCost = 20.0f;

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
        // Latched channels are per level; dead zones are not cleared here - each DeadStone owns its
        // own and drops it on detach, which keeps it independent of script attach order.
        Signal.Clear();
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
    /// <summary>How much ink a creature has taken lately, by entity id. Drowning something is not a
    /// graze - it has to be covered, which is what makes a kill deliberate instead of accidental.</summary>
    private readonly Dictionary<uint, float> _soaked = new();

    /// <summary>Segments that must land on a creature before it drowns.</summary>
    public float SmotherHits = 3.0f;
    /// <summary>How fast coverage drains away, so a stroke that merely clips something on the way
    /// past does not bank progress toward killing it later.</summary>
    public float SoakDecayPerSecond = 1.2f;

    private void SmotherUnder(Vector2 a, Vector2 b)
    {
        Vector2 mid = (a + b) * 0.5f;
        // Drawing is not quiet. Anything alive nearby notices, whether or not this stroke touches it.
        Creature.InkDrawnAt(mid);

        // Sample the WHOLE segment, not just its midpoint. Testing one point meant a fast cursor could
        // sweep a stroke straight through a creature and miss it, which read as the kill "not working".
        float reach = Thickness + 0.55f;
        HashSet<uint> hitThisSegment = new();
        foreach (Vector2 at in new[] { a, mid, b })
        {
            foreach (Entity e in Physics2D.OverlapCircle(at, reach))
            {
                if (e.Id == Self.Id || IsInk(e.Id) || IsPetrified(e.Id)) { continue; }
                if (!Creature.IsCreature(e.Id)) { continue; }
                hitThisSegment.Add(e.Id);
            }
        }

        foreach (uint id in hitThisSegment)
        {
            float soak = (_soaked.TryGetValue(id, out float s) ? s : 0.0f) + 1.0f;
            _soaked[id] = soak;
            if (soak < SmotherHits) { continue; }        // not covered yet - keep drawing over it
            if (Aether < SmotherCost) { continue; }      // cannot afford to unmake it - it lives
            if (Creature.TrySmother(id))
            {
                Aether = Math.Max(0.0f, Aether - SmotherCost);
                _soaked.Remove(id);
            }
        }
    }

    /// <summary>Coverage bleeds off, so smothering has to be one committed act rather than the sum of
    /// every stroke that ever brushed past.</summary>
    private void DecaySoak(float deltaTime)
    {
        if (_soaked.Count == 0) { return; }
        float d = SoakDecayPerSecond * deltaTime;
        List<uint> dry = new();
        foreach (uint id in new List<uint>(_soaked.Keys))
        {
            float v = _soaked[id] - d;
            if (v <= 0.0f) { dry.Add(id); } else { _soaked[id] = v; }
        }
        foreach (uint id in dry) { _soaked.Remove(id); }
    }

    public override void OnUpdate(float deltaTime)
    {
        AetherMax = MaxAether;
        _t += deltaTime;
        _dripCd -= deltaTime;
        DecaySoak(deltaTime);

        // 1. Drawing input -> lay evenly spaced segments along the cursor's path.
        if (!GameState.Won)
        {
            if (Input.IsMousePressed(MouseButton.Left))
            {
                _hasLast = false;
            }
            if (Input.IsMouseReleased(MouseButton.Left))
            {
                EndStroke();
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

            // Ink that found nothing to hold onto SLIDES OFF. It used to just fade in place, which is
            // indistinguishable from a stroke timing out - or from the game losing the input. Ink that
            // visibly sags and drops away is never read as a bug; it is read as a wall you missed.
            float sag = s.Anchored ? 0.0f : 2.8f * s.Age * s.Age;
            Vector2 a = new(s.A.X, s.A.Y - sag);
            Vector2 b = new(s.B.X, s.B.Y - sag);
            Emit(a, b, Thickness * grow, s.Anchored ? alpha : alpha * 0.85f, glow,
                 s.Anchored ? KindInk : KindGhost);

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
                Vector2 mid = (a + b) * 0.5f;
                Vector3 tint = s.Anchored ? InkLightColor : GhostLightColor;
                Lighting2D.SubmitLight(mid, InkLightRadius, tint, InkLightIntensity * alpha * glow);
            }
            minX = Math.Min(minX, Math.Min(a.X, b.X));
            minY = Math.Min(minY, Math.Min(a.Y, b.Y));
            maxX = Math.Max(maxX, Math.Max(a.X, b.X));
            maxY = Math.Max(maxY, Math.Max(a.Y, b.Y));
            _segs[i] = s;
        }

        // Set ink draws in the same pass, flagged ghost = -1 so the shader renders it as dull rock
        // instead of glowing ink. It never ages, so it is packed straight through at full alpha.
        for (int i = 0; i < _petrified.Count; i++)
        {
            Seg s = _petrified[i];
            Emit(s.A, s.B, Thickness * 1.15f, 1.0f, 0.0f, KindSet);
            minX = Math.Min(minX, Math.Min(s.A.X, s.B.X));
            minY = Math.Min(minY, Math.Min(s.A.Y, s.B.Y));
            maxX = Math.Max(maxX, Math.Max(s.A.X, s.B.X));
            maxY = Math.Max(maxY, Math.Max(s.A.Y, s.B.Y));
        }

        // 3. The anchor guide rides in the same pass, so the cursor's verdict is drawn with the ink it
        //    is predicting. Suppressed once the level is won - there is nothing left to build.
        if (!GameState.Won)
        {
            // A cursor outside the game view projects to nonsense (thousands of units out in the
            // editor, where the mouse spends most of its time over panels). Nothing to guide there, and
            // an absurd point would blow the pass quad up to cover the world.
            Vector3 c = Camera.ScreenToWorld(Input.MousePosition);
            Vector2 cursor = new(c.X, c.Y);
            Vector3 here = Self.Position;
            if (Vector2.DistanceSquared(cursor, new Vector2(here.X, here.Y)) < 60.0f * 60.0f)
            {
                PackGuide(cursor, deltaTime, ref minX, ref minY, ref maxX, ref maxY);
            }
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
        _strokeLaid++;
        if (anchored) { _strokeHeld++; }
        else if (InDeadZone(mid)) { _strokeHitDeadRock = true; }
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
        // 0. Dead stone refuses ink outright, however solid it looks. This is how a level says
        //    "not here" without a wall - you can see the route, you just cannot build on it.
        if (InDeadZone(p)) { return false; }

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

    /// <summary>Is there an unbroken run of ink from one point to another? Walks the stroke graph -
    /// live spans and set ones both conduct - joining segments whose ends meet. This is what lets ink
    /// be a WIRE as well as a floor: a socket can ask "am I joined to that source?" and the puzzle
    /// becomes the shape you draw rather than the ground you stand on.</summary>
    public bool HasInkPath(Vector2 from, Vector2 to, float reach)
    {
        List<Seg> all = new(_segs.Count + _petrified.Count);
        for (int i = 0; i < _segs.Count; i++) { if (_segs[i].Anchored) { all.Add(_segs[i]); } }
        all.AddRange(_petrified);
        if (all.Count == 0) { return false; }

        float r2 = reach * reach;
        float joinR2 = (Spacing + Thickness * 2.0f + 0.35f);
        joinR2 *= joinR2;

        bool[] seen = new bool[all.Count];
        Stack<int> open = new();
        for (int i = 0; i < all.Count; i++)
        {
            if (Vector2.DistanceSquared(all[i].A, from) <= r2 || Vector2.DistanceSquared(all[i].B, from) <= r2)
            {
                seen[i] = true;
                open.Push(i);
            }
        }

        while (open.Count > 0)
        {
            Seg s = all[open.Pop()];
            if (Vector2.DistanceSquared(s.A, to) <= r2 || Vector2.DistanceSquared(s.B, to) <= r2)
            {
                return true;
            }
            for (int j = 0; j < all.Count; j++)
            {
                if (seen[j]) { continue; }
                Seg t = all[j];
                if (Vector2.DistanceSquared(s.B, t.A) <= joinR2 || Vector2.DistanceSquared(s.B, t.B) <= joinR2 ||
                    Vector2.DistanceSquared(s.A, t.A) <= joinR2 || Vector2.DistanceSquared(s.A, t.B) <= joinR2)
                {
                    seen[j] = true;
                    open.Push(j);
                }
            }
        }
        return false;
    }

    /// <summary>Regions where ink refuses to set - see <see cref="DeadStone"/>. Registered as world
    /// rects so the anchor test can reject them without knowing what a DeadStone is.</summary>
    /// Keyed by the owning entity so each zone lives and dies with its DeadStone. A global "clear on
    /// level load" looked tidier but depended on the ink attaching before every DeadStone did - and
    /// when it attached after them instead, it wiped the zones and dead rock silently took ink again.
    private static readonly Dictionary<uint, Vector4> s_deadZones = new(); // id -> (minX, minY, maxX, maxY)

    public static void AddDeadZone(uint id, Vector4 rect) => s_deadZones[id] = rect;
    public static void RemoveDeadZone(uint id) => s_deadZones.Remove(id);

    private static bool InDeadZone(Vector2 p)
    {
        foreach (Vector4 r in s_deadZones.Values)
        {
            if (p.X >= r.X && p.X <= r.Z && p.Y >= r.Y && p.Y <= r.W) { return true; }
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------
    // The anchor guide.
    //
    // Ink either bites stone or it does not, and that rule used to be entirely invisible: you found
    // out by drawing and watching the stroke crumble, which reads as the game dropping your input
    // rather than as a rule you broke. Missing a wall by a hand's width looked exactly like missing it
    // by a mile, and both looked like a bug.
    //
    // So the verdict is now stated BEFORE you commit anything. The cursor carries it every frame, and
    // when the answer is "no" but there is stone nearby it draws the gap you actually have to close -
    // the near miss becomes a measurable distance instead of a mystery. Everything here is pure
    // decoration: it goes straight into the ink pass's segment buffer and never becomes a collider, a
    // light or an occluder.
    // ---------------------------------------------------------------------------------------------

    /// <summary>Segment kinds understood by ink_field.slang's <c>ghost</c> channel.</summary>
    private const float KindInk = 0.0f;
    private const float KindGhost = 1.0f;
    private const float KindSet = -1.0f;
    private const float KindGuideHold = 2.0f;   // cursor: this will hold
    private const float KindGuideMiss = 3.0f;   // cursor: this will not hold
    private const float KindGuideTether = 4.0f; // the gap between the cursor and the nearest stone

    // The marks are drops and beads, so each one carries its own width; there is no single line weight.
    // Nothing below about 0.045 survives the shader's hard pixel edges at 16 px/unit - it breaks up and
    // reads as the guide itself glitching.

    /// <summary>Sample offsets for the nearest-stone search, ordered nearest-first so the walk stops as
    /// soon as it finds rock. Built once from <see cref="HintReach"/>.</summary>
    private Vector2[]? _probe;

    private Vector2[] BuildProbe()
    {
        const float Step = 0.15f;
        int n = (int)MathF.Ceiling(HintReach / Step);
        List<Vector2> pts = new();
        for (int iy = -n; iy <= n; iy++)
        {
            for (int ix = -n; ix <= n; ix++)
            {
                Vector2 d = new(ix * Step, iy * Step);
                if (d.LengthSquared() <= HintReach * HintReach) { pts.Add(d); }
            }
        }
        pts.Sort((a, b) => a.LengthSquared().CompareTo(b.LengthSquared()));
        return pts.ToArray();
    }

    private Vector2 _stoneAt, _stoneFor;
    private bool _stoneFound;
    private float _stoneAge = 999.0f;

    /// <summary>Nearest solid tile point to <paramref name="p"/> within <see cref="HintReach"/>. The
    /// probe is sorted nearest-first, so this returns on the first hit instead of scanning the disc.
    ///
    /// Cached across frames: the miss case walks the whole disc before giving up, which is several
    /// hundred tile queries, and out in open air that is EVERY frame. The tether only has to keep up
    /// with a hand moving a mouse, so it is re-solved when the cursor has actually moved or the answer
    /// has gone stale - never once per frame for a cursor sitting still.</summary>
    private bool TryFindNearestStone(Vector2 p, float deltaTime, out Vector2 stone)
    {
        _stoneAge += deltaTime;
        if (_stoneAge < 0.1f && Vector2.DistanceSquared(p, _stoneFor) < 0.02f)
        {
            stone = _stoneAt;
            return _stoneFound;
        }

        _stoneAge = 0.0f;
        _stoneFor = p;
        _probe ??= BuildProbe();
        foreach (Vector2 d in _probe)
        {
            Vector2 q = new(p.X + d.X, p.Y + d.Y);
            if (Physics2D.IsPointSolid(q)) { _stoneAt = q; stone = q; return _stoneFound = true; }
        }
        _stoneAt = p;
        stone = p;
        return _stoneFound = false;
    }

    private void Emit(Vector2 a, Vector2 b, float width, float alpha, float glow, float kind)
    {
        _buf.Add(new Vector4(a.X, a.Y, b.X, b.Y));
        _buf.Add(new Vector4(width, alpha, glow, kind));
    }

    /// <summary>Draw the cursor's verdict and, on a near miss, the gap to the stone it failed to reach.
    /// Returns the world AABB of everything it emitted so the caller can grow the pass quad.</summary>
    private void PackGuide(Vector2 cursor, float deltaTime, ref float minX, ref float minY, ref float maxX, ref float maxY)
    {
        bool dead = InDeadZone(cursor);
        bool hold = !dead && EvaluateAnchor(cursor);
        float kind = hold ? KindGuideHold : KindGuideMiss;

        // The marks are made OF INK, not out of targeting chrome. A crosshair and a set of corner
        // brackets said the right thing but said it in another game's voice - sci-fi HUD furniture
        // floating in a cave about wet black substance. So the whole vocabulary is drawn from what ink
        // physically does on a surface:
        //
        //   holds    - a drop hangs at the point, swollen and about to fall. It has found something.
        //   refused  - the ink BEADS: it cannot wet this, so it breaks into droplets and scatters off
        //              the point, leaving the middle empty. Nothing is holding it, and it looks it.
        //   dead     - the same beading, struck through, the way you cross out a written word.
        //
        // Both states still differ in size, motion and colour as well as shape, so the answer stays
        // legible at a glance and to a player who cannot separate the hues.
        float radius = hold ? 0.16f : 0.34f;
        if (hold)
        {
            // A drop gathering at the nib: round and heavy at the point, tapering to a thread above.
            // Quiet on purpose - this is the state the cursor is in most of the time, and chrome you
            // learn to stop seeing costs the refused state its impact.
            float swell = 1.0f + 0.10f * MathF.Sin(_t * 2.4f); // it breathes, the way a hanging drop does
            Emit(cursor, cursor + new Vector2(0.0f, 0.045f), 0.105f * swell, 1.0f, 1.0f, kind);
            Emit(cursor + new Vector2(0.0f, 0.05f), cursor + new Vector2(0.0f, 0.14f), 0.055f, 0.95f, 1.0f, kind);
            Emit(cursor + new Vector2(0.0f, 0.15f), cursor + new Vector2(0.0f, 0.26f), 0.022f, 0.8f, 1.0f, kind);
        }
        else
        {
            // Beading. Seven droplets thrown off the point, each drifting out and easing back on its own
            // cycle - ink recoiling from something it cannot take hold of.
            for (int i = 0; i < 7; i++)
            {
                float a = (i / 7.0f) * MathF.Tau + 0.4f * MathF.Sin(_t * 0.7f + i);
                float phase = 0.5f + 0.5f * MathF.Sin(_t * 2.6f + i * 1.7f);
                float r = radius * (0.62f + 0.38f * phase);
                float w = 0.048f + 0.030f * (1.0f - phase); // a bead fattens as it slows at the far end
                Vector2 at = cursor + new Vector2(MathF.Cos(a), MathF.Sin(a)) * r;
                Emit(at, at + new Vector2(0.012f, 0.0f), w, 1.0f, 1.0f, kind);
            }
        }

        if (dead)
        {
            // Dead rock is a different refusal from empty air: there is no gap to close, the stone
            // simply will not take ink. Strike it through, the way you cross out a written word, so the
            // player stops hunting for a better angle into it.
            float d = radius * 0.95f;
            Emit(cursor + new Vector2(-d, -d * 0.35f), cursor + new Vector2(d, d * 0.35f), 0.05f, 1.0f, 1.0f, kind);
        }
        else if (!hold && TryFindNearestStone(cursor, deltaTime, out Vector2 stone))
        {
            // The whole point of the guide: you missed by THIS much. A run of drips straining toward the
            // rock turns "it just doesn't work here" into a distance you can see and close - and it is
            // ink doing the straining, not a dotted line drawn over the top of the game.
            Vector2 d = stone - cursor;
            float len = d.Length();
            if (len > 0.01f)
            {
                Vector2 dir = d / len;
                Vector2 nrm = new(-dir.Y, dir.X);
                Vector2 from = cursor + dir * radius;
                float span = MathF.Max(0.0f, len - radius);
                const float Step = 0.30f; // far enough apart that the drips stay drips, not a rod
                float o = (_t * 0.6f) % Step; // the drips creep toward the stone
                for (float s = span - o; s > 0.0f; s -= Step)
                {
                    // Bigger and steadier the closer they get: the trail reads as reaching for the rock
                    // rather than as leaking away from the cursor.
                    float k = s / MathF.Max(span, 0.001f);
                    float w = 0.026f + 0.026f * (1.0f - k);
                    Vector2 at = from + dir * s + nrm * MathF.Sin(s * 6.0f + _t * 1.6f) * 0.035f;
                    Emit(at, at + dir * 0.012f, w, 1.0f, 1.0f, KindGuideTether);
                }
                // A wet smear where the ink WANTS to land, so the eye finishes on the rock rather than
                // trailing off into the dark next to it.
                Emit(stone - nrm * 0.10f, stone + nrm * 0.10f, 0.045f, 1.0f, 1.0f, KindGuideTether);
            }
        }

        float pad = radius + HintReach;
        minX = Math.Min(minX, cursor.X - pad);
        minY = Math.Min(minY, cursor.Y - pad);
        maxX = Math.Max(maxX, cursor.X + pad);
        maxY = Math.Max(maxY, cursor.Y + pad);
    }

    // --- The hint line ----------------------------------------------------------------------------
    // Said out loud only when a whole stroke came to nothing, and only the first few times: the guide
    // is meant to teach the rule, and once it has, repeating it is nagging.

    private static string s_hint = "";
    private static float s_hintUntil;
    private static int s_hintsShown;

    /// <summary>What the HUD should print under the well right now, or empty.</summary>
    public static string Hint => Time.UnscaledTime < s_hintUntil ? s_hint : "";

    private int _strokeLaid;
    private int _strokeHeld;
    private bool _strokeHitDeadRock;

    private void EndStroke()
    {
        int laid = _strokeLaid;
        bool held = _strokeHeld > 0;
        bool deadRock = _strokeHitDeadRock;
        _strokeLaid = 0;
        _strokeHeld = 0;
        _strokeHitDeadRock = false;

        // Nothing to explain unless the player spent a real stroke and got nothing solid for it.
        if (laid < 2 || held || s_hintsShown >= 3 || Time.UnscaledTime < s_hintUntil) { return; }

        s_hint = deadRock ? "Dead rock will not take ink." : "Ink has to touch stone to hold.";
        s_hintUntil = Time.UnscaledTime + 3.0f;
        s_hintsShown++;
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
