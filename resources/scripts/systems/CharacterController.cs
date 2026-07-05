using System;
using System.Numerics;
using AetherCore.Managed;

namespace AetherScripts.Systems;

/// <summary>
/// Camera-relative character movement with momentum, shortest-path turning, and
/// animation state driven by speed. Ported from character_controller.das; the das
/// module globals are now instance fields, so each controller has independent
/// momentum (fixing the shared-state bug when multiple characters use it).
/// </summary>
public sealed class CharacterController
{
    public float WalkSpeed = 10.0f;
    public float SprintSpeed = 15.0f;
    public float Acceleration = 50.0f;
    public float Deceleration = 50.0f;
    public float RotationSpeed = 14.0f;

    private float _velX;
    private float _velZ;
    private float _yaw;
    private int _currentClip = -1;

    private static float MoveTowards(float current, float target, float maxDelta)
    {
        if (MathF.Abs(target - current) <= maxDelta)
        {
            return target;
        }
        return current + (target > current ? maxDelta : -maxDelta);
    }

    public void Update(Entity player, float dt, int idleClip, int walkClip, float inputX, float inputZ, CameraId cam,
        bool isSprinting, int runClip)
    {
        if (!player.IsValid)
        {
            return;
        }

        // 1. Camera-relative basis flattened to the XZ plane (orbit yaw).
        Vector3 camFwd = new(0.0f, 0.0f, -1.0f);
        Vector3 camRight = new(1.0f, 0.0f, 0.0f);
        if (cam.Value != 0)
        {
            float yaw = Camera.GetYaw(cam) * 0.01745329252f;
            camFwd = new Vector3(-MathF.Sin(yaw), 0.0f, -MathF.Cos(yaw));
            camRight = new Vector3(MathF.Cos(yaw), 0.0f, -MathF.Sin(yaw));
        }
        float fLen = MathF.Sqrt(camFwd.X * camFwd.X + camFwd.Z * camFwd.Z);
        float rLen = MathF.Sqrt(camRight.X * camRight.X + camRight.Z * camRight.Z);
        if (fLen > 0.001f) { camFwd.X /= fLen; camFwd.Z /= fLen; }
        if (rLen > 0.001f) { camRight.X /= rLen; camRight.Z /= rLen; }

        // 2. Target velocity from input (normalized so diagonals aren't faster).
        float moveDirX = camFwd.X * inputZ + camRight.X * inputX;
        float moveDirZ = camFwd.Z * inputZ + camRight.Z * inputX;
        float moveLen = MathF.Sqrt(moveDirX * moveDirX + moveDirZ * moveDirZ);

        float targetVelX = 0.0f;
        float targetVelZ = 0.0f;
        if (moveLen > 0.001f)
        {
            float baseSpeed = isSprinting ? SprintSpeed : WalkSpeed;
            targetVelX = moveDirX / moveLen * baseSpeed;
            targetVelZ = moveDirZ / moveLen * baseSpeed;
        }

        // 3. Momentum.
        float accel = (moveLen > 0.001f ? Acceleration : Deceleration) * dt;
        _velX = MoveTowards(_velX, targetVelX, accel);
        _velZ = MoveTowards(_velZ, targetVelZ, accel);

        // 4. Apply movement.
        Vector3 pos = player.Position;
        pos.X += _velX * dt;
        pos.Z += _velZ * dt;

        // 5. Shortest-path yaw lerp toward movement direction.
        float actualSpeed = MathF.Sqrt(_velX * _velX + _velZ * _velZ);
        if (actualSpeed > 0.1f)
        {
            float targetYaw = MathF.Atan2(_velX, _velZ) * 57.2958f;
            float delta = targetYaw - _yaw;
            while (delta > 180.0f) { delta -= 360.0f; }
            while (delta < -180.0f) { delta += 360.0f; }
            _yaw += delta * MathF.Min(1.0f, RotationSpeed * dt);
        }

        player.SetTransform(pos, new Vector3(0.0f, _yaw, 0.0f), player.Scale);

        // 6. Animation state.
        bool isMoving = actualSpeed > 0.2f;
        int targetClip = isMoving ? (isSprinting && runClip >= 0 ? runClip : walkClip) : idleClip;
        if (targetClip >= 0 && targetClip != _currentClip)
        {
            Animation.SetClip(player, targetClip);
            _currentClip = targetClip;
        }

        // 7. Playback speed synced to movement (anti ice-skating).
        if (isMoving)
        {
            float baseSpeed = isSprinting ? SprintSpeed : WalkSpeed;
            float animSpeed = 0.6f + actualSpeed / baseSpeed * 0.6f;
            Animation.SetPlaybackSpeed(player,
                isSprinting && runClip >= 0 ? Math.Clamp(animSpeed * 1.1f, 0.8f, 1.4f) : Math.Clamp(animSpeed, 0.6f, 1.2f));
        }
        else
        {
            Animation.SetPlaybackSpeed(player, 1.0f);
        }
    }
}
