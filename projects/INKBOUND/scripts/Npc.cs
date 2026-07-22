using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Gives a dialogue NPC a little life: it turns to face the player and gently bobs in place.
/// Pair with a SpriteRenderer for the body and a DialogueTrigger for the conversation (see
/// Npc.prefab.toml). Purely cosmetic - the DialogueTrigger owns the interaction and firing.</summary>
public sealed class Npc : EntityScript
{
    public bool FacePlayer = true;
    public float BobAmount = 0.06f; // world units
    public float BobSpeed = 2.2f;

    private float _baseY;
    private float _t;

    public override void OnAttach()
    {
        _baseY = Self.Position.Y;
    }

    public override void OnUpdate(float dt)
    {
        // Idle bob around the spawn height so the wraith never sits perfectly still.
        _t += dt * BobSpeed;
        Vector3 pos = Self.Position;
        pos.Y = _baseY + MathF.Sin(_t) * BobAmount;
        Self.Position = pos;

        if (FacePlayer && PlayerController.Instance is { } player)
        {
            // Face whichever side the player is on (sprite art faces right by default).
            SpriteRenderer.SetFlipX(Self, player.Self.Position.X < Self.Position.X);
        }
    }
}
