using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The signature mechanic: hold the left mouse button and move the cursor to
/// paint a glowing ink platform wherever you point - no reach limit. Ink only
/// holds where it can anchor to real geometry or a crystal (see InkBlock); drawn
/// in open air it crumbles, so you extend ledges and bridge real gaps rather than
/// hovering across the level. Drawing drains an aether meter (shown on the HUD)
/// that refills only on actual ground or from crystals - never on your own ink,
/// so a hover can never self-sustain. Attach alongside PlayerController.
/// </summary>
public sealed class AetherInk : EntityScript
{
    public float MaxAether = 100.0f;
    public float DrainPerBlock = 5.0f;
    public float RefillPerSecond = 34.0f;
    /// <summary>World distance the cursor must move before the next ink block lays.</summary>
    public float BlockSpacing = 0.34f;

    // Read by HudController for the meter bar.
    public static float Aether;
    public static float AetherMax = 100.0f;

    private Vector2 _lastInk;
    private bool _drewSincePress;

    public override void OnAttach()
    {
        Aether = MaxAether;
        AetherMax = MaxAether;
        _lastInk = new Vector2(Self.Position.X, Self.Position.Y);
    }

    public override void OnUpdate(float deltaTime)
    {
        AetherMax = MaxAether;
        if (GameState.Won)
        {
            return;
        }

        // Fresh press restarts the trail so the first block lays immediately.
        if (Input.IsMousePressed(MouseButton.Left))
        {
            _drewSincePress = false;
        }

        bool drawing = Input.IsMouseDown(MouseButton.Left) && Aether > 0.0f;
        if (drawing)
        {
            // Draw wherever the cursor points - no reach limit. Whether the ink
            // actually holds is decided by InkBlock's anchoring, not by distance.
            Vector3 cursor = Camera.ScreenToWorld(Input.MousePosition);
            Vector2 point = new(cursor.X, cursor.Y);
            float moved = Vector2.Distance(point, _lastInk);
            if (!_drewSincePress || moved >= BlockSpacing)
            {
                Scene.Instantiate("InkBlock", new Vector3(point.X, point.Y, 0.0f));
                Aether = Math.Max(0.0f, Aether - DrainPerBlock);
                _lastInk = point;
                _drewSincePress = true;
            }
        }
        else if (IsOnRealGround())
        {
            // Refill only on real terrain (or crystals, handled by AetherCrystal) -
            // never while standing on your own ink, so hovering can't self-sustain.
            Aether = Math.Min(MaxAether, Aether + RefillPerSecond * deltaTime);
        }
    }

    /// <summary>Grounded on ACTUAL terrain, not on conjured ink.</summary>
    private bool IsOnRealGround()
    {
        Vector3 pos = Self.Position;
        for (float offset = -0.28f; offset <= 0.28f; offset += 0.56f)
        {
            RaycastHit2D hit = Physics2D.Raycast(new Vector2(pos.X + offset, pos.Y), new Vector2(0.0f, -1.0f), 0.85f);
            if (hit.DidHit && !InkBlock.IsInk(hit.Entity.Id))
            {
                return true;
            }
        }
        return false;
    }
}
