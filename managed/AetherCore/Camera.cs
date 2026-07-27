using System.Numerics;

namespace AetherCore;

public enum CameraMode
{
    Orbit = 0,
    Free = 1,
}

public enum CameraProjection
{
    Perspective = 0,
    Orthographic = 1,
}

/// <summary>
/// Camera creation and control. A camera is an ENTITY carrying a CameraComponent
/// (orbit cameras additionally carry an OrbitCameraComponent), so a "camera handle"
/// is simply its <see cref="Entity"/>. Assign a camera entity to a script's public
/// <see cref="Entity"/> field to reference it, or spawn one with CreateOrbit/CreateFree.
/// </summary>
public static class Camera
{
    /// <summary>Spawn an orbit (third-person) camera entity. It appears in the
    /// hierarchy; tag it as the scene view with <see cref="SetMain"/>.</summary>
    public static Entity CreateOrbit(Vector3 position, Vector3 target, float fovDegrees)
        => new(Native.aether_camera_create_orbit(position, target, fovDegrees));

    /// <summary>Spawn a free camera entity whose pose the script drives directly.</summary>
    public static Entity CreateFree(Vector3 position, float fovDegrees)
        => new(Native.aether_camera_create_free(position, fovDegrees));

    /// <summary>Spawn an orthographic camera looking down the negative Z axis.</summary>
    public static Entity CreateOrthographic(Vector3 position, float height)
        => new(Native.aether_camera_create_orthographic(position, height));

    /// <summary>Make <paramref name="camera"/> the scene's single main camera.</summary>
    public static void SetMain(Entity camera) => Native.aether_camera_set_main(camera.Id);

    /// <summary>The scene's current main-camera entity (invalid if none).</summary>
    public static Entity Main => new(Native.aether_camera_get_main());

    public static void SetMode(Entity camera, CameraMode mode) => Native.aether_camera_set_mode(camera.Id, (int)mode);

    public static void SetPerspective(Entity camera, float fovDegrees) => Native.aether_camera_set_perspective(camera.Id, fovDegrees);

    public static void SetOrthographic(Entity camera, float height) => Native.aether_camera_set_orthographic(camera.Id, height);

    public static CameraProjection GetProjection(Entity camera) => (CameraProjection)Native.aether_camera_get_projection(camera.Id);

    public static float GetOrthographicHeight(Entity camera) => Native.aether_camera_get_orthographic_height(camera.Id);

    public static void SetPosition(Entity camera, Vector3 position) => Native.aether_camera_set_position(camera.Id, position);

    public static void SetYawPitch(Entity camera, float yaw, float pitch) => Native.aether_camera_set_yaw_pitch(camera.Id, yaw, pitch);

    public static void SetTarget(Entity camera, Vector3 target) => Native.aether_camera_set_target(camera.Id, target);

    public static void SetOrbital(Entity camera, float yaw, float pitch, float distance)
        => Native.aether_camera_set_orbital(camera.Id, yaw, pitch, distance);

    public static float GetYaw(Entity camera) => Native.aether_camera_get_yaw(camera.Id);

    public static Vector3 GetForward(Entity camera) => Native.aether_camera_get_forward(camera.Id);

    public static Vector3 GetRight(Entity camera) => Native.aether_camera_get_right(camera.Id);

    /// <summary>Convert a cursor/screen position (as from <see cref="Input.MousePosition"/>)
    /// to a world point on the 2D plane (z = 0), using the current main camera.
    /// Exact for orthographic 2D cameras. Handles both the editor viewport and a
    /// shipped fullscreen window.</summary>
    public static Vector3 ScreenToWorld(Vector2 screenPos) => Native.aether_camera_screen_to_world(screenPos);

    /// <summary>Project a world position to render-target pixels (top-left origin),
    /// matching <see cref="Input.MousePosition"/> and the UI canvas space. Returns
    /// (-1, -1) when the position is behind the camera, so a caller can cull with one
    /// comparison.</summary>
    /// <remarks>
    /// To drive a UI element from this, anchor it to the top-left first
    /// (<c>Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero)</c>): <c>Ui.SetRect</c> takes
    /// x/y as an offset FROM the element's anchor, and elements default to
    /// centre-anchored, so passing these pixels to a freshly created element without
    /// re-anchoring puts it half a screen away from the intended point.
    /// </remarks>
    public static Vector2 WorldToScreen(Vector3 worldPos) => Native.aether_camera_world_to_screen(worldPos);
}
