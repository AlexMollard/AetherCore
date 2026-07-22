using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The signature mechanic: hold the left mouse button and move the cursor to paint a glowing ink
/// stroke wherever you point - no reach limit. The visual is a single continuous wet-ink layer
/// rendered entirely in a shader (see the Ink field / shaders/ink_field.slang): this script only
/// owns the stroke geometry, feeding every live segment to Ink each frame. Physics is separate and
/// proper - each anchored segment gets one capsule collider aligned to the segment, so a stroke
/// reads as a smooth rounded ledge rather than a pile of boxes.
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
    public float Thickness = 0.22f;
    public float Lifetime = 5.0f;
    public float FadeTime = 1.6f;
    /// <summary>How close to real geometry (or a crystal) a segment must be to hold.</summary>
    public float AnchorRadius = 1.7f;

    // Read by HudController for the meter bar.
    public static float Aether;
    public static float AetherMax = 100.0f;

    private static readonly Vector4 BodyColor = new(0.05f, 0.09f, 0.13f, 1.0f); // dark ink
    private static readonly Vector4 RimColor = GameSettings.Accent;             // cyan wet rim

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

    public override void OnAttach()
    {
        Aether = MaxAether;
        AetherMax = MaxAether;
        _segs.Clear();
        s_inkColliders.Clear(); // stale ids from a previous level never carry over
        _hasLast = false;
        Ink.SetColors(BodyColor, RimColor);
    }

    public override void OnDetach()
    {
        // Leaving the level: clear the field so no stray ink renders over the next scene.
        Ink.Clear();
        s_inkColliders.Clear();
    }

    public override void OnUpdate(float deltaTime)
    {
        AetherMax = MaxAether;

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
                        LaySegment(_last, next);
                        Aether = Math.Max(0.0f, Aether - DrainPerSegment);
                        _last = next;
                        gap = Vector2.Distance(point, _last);
                    }
                }
            }
            else if (IsOnRealGround())
            {
                // Refill only on real terrain (crystals handled by AetherCrystal) - never on your own ink.
                Aether = Math.Min(MaxAether, Aether + RefillPerSecond * deltaTime);
            }
        }

        // 2. Age segments, expire the dead (and their colliders), and re-feed the live ones to the
        //    ink field. Rebuilt every frame so the shader always has the current stroke.
        Ink.Clear();
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
            if (s.Anchored)
            {
                float glow = 0.4f + 0.6f * GameSettings.InkGlow; // Ink Glow setting scales the rim
                Ink.AddSegment(s.A, s.B, Thickness * grow, alpha, glow, 0.0f);
            }
            else
            {
                Ink.AddSegment(s.A, s.B, Thickness * grow, alpha * 0.85f, 0.5f, 1.0f); // ghost = red
            }
            _segs[i] = s;
        }
    }

    private void LaySegment(Vector2 a, Vector2 b)
    {
        bool anchored = EvaluateAnchor((a + b) * 0.5f);
        Seg s = new() { A = a, B = b, Age = 0.0f, Anchored = anchored };

        if (anchored)
        {
            // Proper collider: one capsule aligned to the segment (local Y is the capsule's long
            // axis, so rotate the entity by the segment angle minus 90 degrees).
            Vector2 mid = (a + b) * 0.5f;
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
    }

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
