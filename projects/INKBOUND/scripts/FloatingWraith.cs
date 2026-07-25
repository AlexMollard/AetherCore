using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A floating wraith. It drifts back and forth adrift in the dark, bobbing up and down, and turns
/// at walls or at the edge of its patrol range - it never falls, so it happily crosses gaps a ground
/// enemy can't. Touching it from the side or below sends the player back to the last checkpoint;
/// dropping onto it from above dispels it (a stomp that bounces the player, same as the ground enemy).
/// A kinematic trigger sensor: it passes through the player rather than shoving them, and is moved
/// purely by velocity so Box2D keeps the sensor sweeps clean. Pairs with the slime sprite tinted a
/// spectral violet. Attach to a kinematic body whose collider is a trigger.
/// </summary>
public sealed class FloatingWraith : EntityScript
{
    /// <summary>Horizontal cruise speed (world units/sec).</summary>
    public float DriftSpeed = 1.6f;
    /// <summary>Max distance it drifts from its spawn X before turning (it also turns at walls).</summary>
    public float PatrolRange = 4.0f;
    /// <summary>Vertical bob half-range (world units).</summary>
    public float BobAmplitude = 0.55f;
    /// <summary>Seconds per bob cycle.</summary>
    public float BobPeriod = 2.4f;
    /// <summary>Shrink-and-fade time after a stomp before the wraith vanishes.</summary>
    public float DispelSeconds = 0.4f;

    private float _spawnX;
    private float _baseY;
    private float _direction = -1.0f;
    private float _t;
    private float _dispel = -1.0f;
    private Vector2 _baseSize;
    private Vector4 _baseTint;

    public override void OnAttach()
    {
        Physics2D.SetTrigger(Self, true); // force sensor at runtime - it never blocks the player
        Physics2D.EnableEvents(Self);
        _spawnX = Self.Position.X;
        _baseY = Self.Position.Y;
        _baseSize = SpriteRenderer.GetPixelSize(Self);
        _baseTint = SpriteRenderer.GetTint(Self);
        Creature.Register(Self.Id, Smother);
    }

    public override void OnDetach() => Creature.Unregister(Self.Id);

    /// <summary>Drowned in ink - the only way it dies. Ink is the weapon; there is no stomp.</summary>
    private void Smother()
    {
        if (_dispel >= 0.0f) { return; }
        _dispel = DispelSeconds;
        CameraFollow.Instance?.AddShake(0.06f);
        Scene.Instantiate("DeathInk", new Vector3(Self.Position.X, Self.Position.Y, 0.0f));
        Log.Info("[INKBOUND] The wraith came apart in the ink.");
    }

    public override void OnUpdate(float deltaTime)
    {
        // Dispel: shrink and fade after a stomp, then remove.
        if (_dispel >= 0.0f)
        {
            _dispel -= deltaTime;
            float k = Math.Clamp(_dispel / DispelSeconds, 0.0f, 1.0f);
            SpriteRenderer.SetPixelSize(Self, _baseSize * (0.35f + 0.65f * k));
            SpriteRenderer.SetTint(Self, new Vector4(_baseTint.X, _baseTint.Y, _baseTint.Z, _baseTint.W * k));
            if (_dispel <= 0.0f)
            {
                Self.Destroy();
            }
            return;
        }

        _t += deltaTime;
        Vector3 pos = Self.Position;

        // Turn at walls and at the patrol-range edge; there is no ledge check - wraiths float over gaps.
        bool wall = Physics2D.Raycast(new Vector2(pos.X, pos.Y), new Vector2(_direction, 0.0f), 0.6f).DidHit;
        bool outOfRange = (pos.X - _spawnX) * _direction > PatrolRange;
        if (wall || outOfRange)
        {
            _direction = -_direction;
        }

        // Kinematic bodies are driven by their ECS transform (the engine pushes it into Box2D each
        // step and never syncs it back), NOT by velocity - so move by writing the position directly.
        // Drift horizontally and bob around the spawn height.
        float omega = (2.0f * MathF.PI) / MathF.Max(0.1f, BobPeriod);
        float x = pos.X + _direction * DriftSpeed * deltaTime;
        float y = _baseY + BobAmplitude * MathF.Sin(_t * omega);
        Self.Position = new Vector3(x, y, pos.Z);
        SpriteRenderer.SetFlipX(Self, _direction > 0.0f);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (player == null || other.Id != player.Self.Id || _dispel >= 0.0f)
        {
            return;
        }

        // Touching it is fatal from any angle. Draw over it instead.
        Log.Info("[INKBOUND] The wraith caught you.");
        player.Die();
    }
}
