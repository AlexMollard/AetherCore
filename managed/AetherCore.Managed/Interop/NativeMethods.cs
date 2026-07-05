using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AetherCore.Managed.Interop;

/// <summary>
/// Source-generated P/Invokes into the engine's C exports. "AetherHost" resolves
/// to the running executable (App.exe), which exports the aether_* functions —
/// no separate native DLL is loaded.
/// </summary>
internal static unsafe partial class Native
{
    private const string Lib = "AetherHost";

    // Called once from Bootstrap.Init (before any script runs, hence before the
    // first P/Invoke) to point "AetherHost" at the running executable.
    internal static void RegisterResolver()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly,
            static (name, _, _) => name == Lib ? NativeLibrary.GetMainProgramHandle() : IntPtr.Zero);
    }

    // ── World / entity / transform ────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial uint aether_entity_create();

    [LibraryImport(Lib)]
    internal static partial void aether_entity_destroy(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_entity_valid(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_mark_transient(uint id);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_set_name(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial int aether_get_name(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_position(uint id);

    // Setters do subtree propagation + physics teleport, so no SuppressGCTransition
    // (that is reserved for trivial, non-blocking leaf calls).
    [LibraryImport(Lib)]
    internal static partial void aether_set_position(uint id, Vector3 pos);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_euler(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_set_euler(uint id, Vector3 euler);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_get_scale(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_set_transform(uint id, Vector3 pos, Vector3 euler, Vector3 scale);

    // ── Input ─────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_down(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_pressed(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_input_key_released(int keyCode);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_input_delta_time();

    // ── Camera ────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial uint aether_camera_create_orbit(Vector3 pos, Vector3 target, float fovDeg);

    [LibraryImport(Lib)]
    internal static partial uint aether_camera_create_free(Vector3 pos, float fovDeg);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_main(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_mode(uint id, int mode);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_position(uint id, Vector3 pos);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_yaw_pitch(uint id, float yaw, float pitch);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_target(uint id, Vector3 target);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_orbital(uint id, float yaw, float pitch, float dist);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_camera_get_yaw(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_camera_get_forward(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_camera_get_right(uint id);

    // ── Physics ───────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_box(uint id, Vector3 halfExtents, int dynamic);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_sphere(uint id, float radius, int dynamic);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_capsule(uint id, float halfHeight, float radius, int dynamic);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_set_linear_velocity(uint id, Vector3 velocity);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_physics_get_linear_velocity(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_physics_get_position(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_physics_get_scale(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_set_debug_enabled(int enabled);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_physics_is_debug_enabled();

    // ── Effects ───────────────────────────────────────────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_effect_set(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial void aether_effect_set_color(uint id, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_effect_set_speed(uint id, float speed);

    [LibraryImport(Lib)]
    internal static partial void aether_effect_set_scale(uint id, float scale);

    [LibraryImport(Lib)]
    internal static partial void aether_effect_set_intensity(uint id, float intensity);

    // ── Renderer / lighting ───────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_render_set_ambient(Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_sun(Vector3 dir, float intensity, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_render_add_point_light(Vector3 pos, Vector3 color, float intensity, float radius, int castsShadow);

    [LibraryImport(Lib)]
    internal static partial void aether_render_add_spot_light(Vector3 pos, Vector3 color, float intensity, float radius, Vector3 dir, float innerAngle, float outerAngle, int castsShadow);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_point_light_position(int idx, Vector3 pos);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_point_light_color(int idx, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_point_light_intensity(int idx, float intensity);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_render_get_point_light_count();

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_spot_light_position(int idx, Vector3 pos);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_spot_light_color(int idx, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_spot_light_intensity(int idx, float intensity);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_render_get_spot_light_count();

    [LibraryImport(Lib)]
    internal static partial void aether_render_clear_lights();

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_sky(Vector3 horizon, Vector3 zenith);

    [LibraryImport(Lib)]
    internal static partial void aether_render_set_sky_void(Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_daynight_set_enabled(int enabled);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_daynight_get_enabled();

    [LibraryImport(Lib)]
    internal static partial void aether_daynight_set_time(float hours);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_daynight_get_time();

    [LibraryImport(Lib)]
    internal static partial void aether_daynight_set_speed(float secondsPerSecond);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_daynight_get_speed();

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_daynight_get_sun_direction();
}
