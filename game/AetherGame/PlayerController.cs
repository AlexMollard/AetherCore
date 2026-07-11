using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A physics-driven capsule character. Demonstrates the unified RigidBody +
/// Collider API together with the newer physics features: axis-locked bodies,
/// impulses, a grounded raycast, and trigger events for pickups.
///
/// Attach it to an entity (a capsule works best) and press Play. WASD moves,
/// Space jumps when grounded, and walking into any trigger volume "collects" it.
///
/// Public fields are exposed in the inspector, so speeds and the capsule size
/// can be tuned per entity without recompiling.
/// </summary>
public sealed class PlayerController : EntityScript
{
    public float MoveSpeed = 6.0f;
    public float JumpImpulse = 6.0f;
    public float CapsuleRadius = 0.5f;
    public float CapsuleHalfHeight = 0.6f;

    private bool _grounded;

    public override void OnAttach()
    {
        // Give the entity a dynamic capsule body, lock its rotation so it stays
        // upright, and start recording overlaps. (A designer can also set the
        // collider up in the editor; this keeps the script self-contained.)
        Physics.AddCapsuleBody(Self, CapsuleHalfHeight, CapsuleRadius, dynamic: true);
        Physics.FreezeRotation(Self, x: true, y: true, z: true);
        Physics.EnableEvents(Self);
    }

    public override void OnUpdate(float deltaTime)
    {
        MoveHorizontally();
        UpdateGrounded();
        TryJump();
        CollectTriggers();
    }

    // WASD sets the horizontal velocity directly while leaving gravity to own the
    // vertical component - a simple, responsive kinematic-feeling move.
    private void MoveHorizontally()
    {
        Vector3 wish = Vector3.Zero;
        if (Input.IsKeyDown(Key.W)) { wish.Z -= 1.0f; }
        if (Input.IsKeyDown(Key.S)) { wish.Z += 1.0f; }
        if (Input.IsKeyDown(Key.A)) { wish.X -= 1.0f; }
        if (Input.IsKeyDown(Key.D)) { wish.X += 1.0f; }
        if (wish != Vector3.Zero) { wish = Vector3.Normalize(wish); }

        Vector3 velocity = Physics.GetLinearVelocity(Self);
        velocity.X = wish.X * MoveSpeed;
        velocity.Z = wish.Z * MoveSpeed;
        Physics.SetLinearVelocity(Self, velocity);
    }

    // Cast a short ray straight down from the capsule centre. A convex ray that
    // starts inside its own shape reports no self-hit, so a hit here is ground;
    // the id guard is a belt-and-braces fallback.
    private void UpdateGrounded()
    {
        float reach = CapsuleHalfHeight + CapsuleRadius + 0.15f;
        RaycastHit hit = Physics.Raycast(Physics.GetPosition(Self), new Vector3(0.0f, -1.0f, 0.0f), reach);
        _grounded = hit.DidHit && hit.Entity.Id != Self.Id;
    }

    private void TryJump()
    {
        if (_grounded && Input.IsKeyPressed(Key.Space))
        {
            Physics.AddImpulse(Self, new Vector3(0.0f, JumpImpulse, 0.0f));
        }
    }

    // Anything whose trigger we started overlapping this frame gets collected.
    private void CollectTriggers()
    {
        foreach (Entity pickup in Physics.GetTriggerEnter(Self))
        {
            Log.Info($"Player collected trigger entity #{pickup.Id}");
            pickup.Destroy();
        }
    }
}
