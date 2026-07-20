using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Side-scroller camera: smoothly tracks the player along X (never backwards
/// past the level start), holds a gentle Y band, and adds a bit of juice -
/// velocity look-ahead so you see where you're going, plus a decaying shake
/// other scripts trigger on impacts (landings, stomps).
/// </summary>
public sealed class CameraFollow : EntityScript
{
    public float MinX = 0.0f;
    public float MaxX = 74.0f;
    public float BaseY = 2.0f;
    public float SmoothSpeed = 6.0f;

    /// <summary>How far ahead of the player the camera leads, in world units.</summary>
    public float LookAhead = 2.4f;

    /// <summary>Reachable by impact scripts (PlayerController, EnemyPatrol).</summary>
    public static CameraFollow? Instance;

    private Entity _player;
    private float _lookAhead; // smoothed current lead offset
    private float _shake;     // current shake amplitude (world units), decays
    private float _shakeTime;

    public override void OnAttach()
    {
        Instance = this;
        _player = Scene.Find("Player");
    }

    public override void OnDetach()
    {
        if (Instance == this)
        {
            Instance = null;
        }
    }

    /// <summary>Kick a screen shake; larger amplitude = harder hit.</summary>
    public void AddShake(float amplitude)
    {
        _shake = MathF.Max(_shake, amplitude);
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_player.IsValid)
        {
            _player = Scene.Find("Player");
            return;
        }

        Vector3 target = _player.Position;
        Vector3 pos = Self.Position;

        // Lead the camera in the direction the player is actually moving.
        float vx = Physics2D.GetLinearVelocity(_player).X;
        float wantLead = Math.Clamp(vx / 7.0f, -1.0f, 1.0f) * LookAhead;
        _lookAhead += (wantLead - _lookAhead) * Math.Clamp(3.0f * deltaTime, 0.0f, 1.0f);

        float wantX = Math.Clamp(target.X + _lookAhead, MinX, MaxX);
        float wantY = Math.Max(BaseY, target.Y * 0.35f + BaseY * 0.65f);
        float t = Math.Clamp(SmoothSpeed * deltaTime, 0.0f, 1.0f);
        pos.X += (wantX - pos.X) * t;
        pos.Y += (wantY - pos.Y) * t;

        // Decaying shake: a soft bump, not a jitter - gentle sine frequencies
        // and a quick fade so it reads as a knock and settles fast.
        if (_shake > 0.0001f)
        {
            _shakeTime += deltaTime;
            pos.X += MathF.Sin(_shakeTime * 26.0f) * _shake;
            pos.Y += MathF.Cos(_shakeTime * 21.0f) * _shake;
            _shake *= MathF.Max(0.0f, 1.0f - 13.0f * deltaTime);
        }

        Self.Position = pos;
    }
}
