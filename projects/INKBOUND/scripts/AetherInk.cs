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
    public float MaxAether = 100.0f;
    public float DrainPerSegment = 4.0f;
    public float RefillPerSecond = 34.0f;
    /// <summary>World distance between segment nodes as the stroke is laid.</summary>
    public float Spacing = 0.5f;
    /// <summary>Ink half-thickness in world units (drives both the shader width and the collider radius).</summary>
    public float Thickness = 0.11f;
    public float Lifetime = 5.0f;
    public float FadeTime = 1.6f;
    /// <summary>How close to real geometry (or a crystal) a segment must be to hold.</summary>
    public float AnchorRadius = 1.7f;

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

    // Live ink-collider ids, so anchoring and "is the player on real ground?" checks can tell
    // conjured ink from actual terrain.
    private static readonly HashSet<uint> s_inkColliders = new();
    public static bool IsInk(uint id) => s_inkColliders.Contains(id);

    private Vector2 _last;
    private bool _hasLast;
    private float _t;
    private float _dripCd; // throttles ink-droplet spawns while drawing

    public override void OnAttach()
    {
        Aether = MaxAether;
        AetherMax = MaxAether;
        _segs.Clear();
        s_inkColliders.Clear(); // stale ids from a previous level never carry over
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
        return true;
    }

    /// <summary>Does this point sit inside solid terrain (a wall/ground tile)? Uses the tilemap tile
    /// solidity so it's correct deep inside a block - the ground's chain colliders are hollow, so an
    /// overlap query only caught the surface edge and let ink be drawn inside. Points in empty cells
    /// (including just above a surface) return false, so you can still draw ledges onto walls.</summary>
    private static bool IsInsideSolid(Vector2 p) => Physics2D.IsPointSolid(p);

    /// <summary>Is real geometry (or a crystal) within reach of this point? Other ink does not count
    /// - a span must ultimately reach the actual world to hold.</summary>
    private bool EvaluateAnchor(Vector2 p)
    {
        uint playerId = Self.Id;
        foreach (Entity e in Physics2D.OverlapCircle(p, AnchorRadius))
        {
            uint id = e.Id;
            if (id == playerId || IsInk(id))
            {
                continue;
            }
            return true; // terrain or crystal within reach
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
