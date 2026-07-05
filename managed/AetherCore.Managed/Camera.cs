using System.Numerics;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>Opaque handle to a camera (0 is none).</summary>
public readonly record struct CameraId(uint Value)
{
    public bool IsValid => Value != 0;
}

public enum CameraMode
{
    Orbit = 0,
    Free = 1,
}

/// <summary>Camera creation and control.</summary>
public static class Camera
{
    public static CameraId CreateOrbit(Vector3 position, Vector3 target, float fovDegrees)
        => new(Native.aether_camera_create_orbit(position, target, fovDegrees));

    public static CameraId CreateFree(Vector3 position, float fovDegrees)
        => new(Native.aether_camera_create_free(position, fovDegrees));

    public static void SetMain(CameraId camera) => Native.aether_camera_set_main(camera.Value);

    public static void SetMode(CameraId camera, CameraMode mode) => Native.aether_camera_set_mode(camera.Value, (int)mode);

    public static void SetPosition(CameraId camera, Vector3 position) => Native.aether_camera_set_position(camera.Value, position);

    public static void SetYawPitch(CameraId camera, float yaw, float pitch) => Native.aether_camera_set_yaw_pitch(camera.Value, yaw, pitch);

    public static void SetTarget(CameraId camera, Vector3 target) => Native.aether_camera_set_target(camera.Value, target);

    public static void SetOrbital(CameraId camera, float yaw, float pitch, float distance)
        => Native.aether_camera_set_orbital(camera.Value, yaw, pitch, distance);

    public static float GetYaw(CameraId camera) => Native.aether_camera_get_yaw(camera.Value);

    public static Vector3 GetForward(CameraId camera) => Native.aether_camera_get_forward(camera.Value);

    public static Vector3 GetRight(CameraId camera) => Native.aether_camera_get_right(camera.Value);
}
