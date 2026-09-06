using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// RETIRED from Sandbox.scene.toml - not attached to any entity there anymore.
/// Kept only as a standalone debug tool: attach it by hand (Add Component >
/// Script > DebugFlyCam) on any camera entity when you need to fly around
/// independent of the real player.
///
/// It was the sandbox's placeholder camera before the engine had a real
/// Character Controller: it drives the camera entity directly and has no
/// body, no collision and no gravity, which is exactly the direct-transform-
/// write pattern <see cref="FirstPersonPlayer"/> exists specifically to avoid
/// for actual player movement. Now that FirstPersonPlayer (on the real
/// Character Controller) is the Player entity's script, this is no longer
/// wired into the scene at all.
///
/// Controls: hold the right mouse button to look around, WASD to move in the
/// look direction, Space/Ctrl for up/down, Shift to move faster. Suppressed
/// entirely while <see cref="SpawnMenu"/> is open (checked via its own
/// IsOpen, not Ui.HasFocus - see that script's file comment for why).
/// </summary>
public sealed class DebugFlyCam : EntityScript
{
    public float MoveSpeed = 8.0f;
    public float BoostMultiplier = 3.0f;
    public float LookSensitivity = 0.15f;

    private float _yaw;
    private float _pitch;

    public override void OnAttach()
    {
        Vector3 euler = Self.EulerDegrees;
        _pitch = euler.X;
        _yaw = euler.Y;
    }

    public override void OnUpdate(float deltaTime)
    {
        if (GetScript<SpawnMenu>() is { IsOpen: true })
        {
            return;
        }

        if (Input.IsMouseDown(MouseButton.Right))
        {
            Vector2 delta = Input.MouseDelta;
            _yaw += delta.X * LookSensitivity;
            _pitch = Math.Clamp(_pitch - delta.Y * LookSensitivity, -89.0f, 89.0f);
            Self.EulerDegrees = new Vector3(_pitch, _yaw, 0.0f);
        }

        Vector3 forward = Camera.GetForward(Self);
        Vector3 right = Camera.GetRight(Self);
        float speed = Input.IsKeyDown(Key.LeftShift) ? MoveSpeed * BoostMultiplier : MoveSpeed;

        Vector3 move = forward * Input.GetAxisRaw(Key.S, Key.W) + right * Input.GetAxisRaw(Key.A, Key.D);
        move.Y += Input.GetAxisRaw(Key.LeftCtrl, Key.Space);

        Self.Position += move * speed * deltaTime;
    }
}
