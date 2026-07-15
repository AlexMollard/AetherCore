using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AetherCore;

/// <summary>
/// Source-generated P/Invokes into the engine's C exports. "AetherHost" resolves
/// to the running executable (App.exe), which exports the aether_* functions -
/// no separate native DLL is loaded.
/// </summary>
internal static unsafe partial class Native
{
    private const string Lib = "AetherHost";

	[LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
	internal static partial void aether_sprite_set_texture(uint id, string path);
	[LibraryImport(Lib)] internal static partial Vector4 aether_sprite_get_tint(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_tint(uint id, Vector4 value);
	[LibraryImport(Lib)] internal static partial Vector2 aether_sprite_get_pixel_size(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_pixel_size(uint id, Vector2 value);
	[LibraryImport(Lib)] internal static partial Vector2 aether_sprite_get_pivot(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_pivot(uint id, Vector2 value);
	[LibraryImport(Lib)] internal static partial float aether_sprite_get_pixels_per_unit(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_pixels_per_unit(uint id, float value);
	[LibraryImport(Lib)] internal static partial int aether_sprite_get_sorting_layer(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_sorting_layer(uint id, int value);
	[LibraryImport(Lib)] internal static partial int aether_sprite_get_order_in_layer(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_order_in_layer(uint id, int value);
	[LibraryImport(Lib)] internal static partial int aether_sprite_get_blend_mode(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_blend_mode(uint id, int value);
	[LibraryImport(Lib)] internal static partial uint aether_sprite_get_flags(uint id);
	[LibraryImport(Lib)] internal static partial void aether_sprite_set_flags(uint id, uint flags);

    // Called once from AetherCore.Interop.Bootstrap.Init (before any script runs,
    // hence before the first P/Invoke) to point "AetherHost" at the running
    // executable. Reachable from the Interop assembly via InternalsVisibleTo.
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

    [LibraryImport(Lib)]
    internal static partial void aether_add_transform(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_has_transform(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_remove_transform(uint id);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_set_name(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial int aether_get_name(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_text(uint id, string text);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_text(uint id, byte* buf, int bufLen);

    // ── UI toolkit (Module 02) ──────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial uint aether_ui_create_canvas();

    [LibraryImport(Lib)]
    internal static partial uint aether_ui_create_image(uint canvas);

    [LibraryImport(Lib)]
    internal static partial uint aether_ui_create_text(uint canvas);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_anchors(uint id, Vector2 min, Vector2 max);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_offsets(uint id, Vector2 min, Vector2 max);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_pivot(uint id, Vector2 pivot);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_rect(uint id, float x, float y, float w, float h);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_color(uint id, Vector4 color);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_font_size(uint id, float pixelSize);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_align(uint id, int h, int v);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_image_color(uint id, Vector4 color);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_image_corner_radius(uint id, float radius);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_image_texture(uint id, string path);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_contains_point(uint id, Vector2 pt);

    [LibraryImport(Lib)]
    internal static partial Vector4 aether_ui_get_rect(uint id);

    // ── Debug draw (Module 08) ──────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_debug_set_enabled(int enabled);

    [LibraryImport(Lib)]
    internal static partial int aether_debug_is_enabled();

    [LibraryImport(Lib)]
    internal static partial void aether_debug_draw_line(Vector3 a, Vector3 b, Vector4 color);

    [LibraryImport(Lib)]
    internal static partial void aether_debug_draw_ray(Vector3 origin, Vector3 dir, Vector4 color);

    [LibraryImport(Lib)]
    internal static partial void aether_debug_draw_sphere(Vector3 center, float radius, Vector4 color);

    [LibraryImport(Lib)]
    internal static partial void aether_debug_draw_box(Vector3 center, Vector3 halfExtents, Vector4 color);

    // ── Time (Module 05) ────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial float aether_time_total();

    [LibraryImport(Lib)]
    internal static partial float aether_time_delta();

    [LibraryImport(Lib)]
    internal static partial long aether_time_frame_count();

    // ── Mouse input (Module 03) ─────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial int aether_input_mouse_down(int btn);

    [LibraryImport(Lib)]
    internal static partial int aether_input_mouse_pressed(int btn);

    [LibraryImport(Lib)]
    internal static partial int aether_input_mouse_released(int btn);

    [LibraryImport(Lib)]
    internal static partial Vector2 aether_input_mouse_pos();

    [LibraryImport(Lib)]
    internal static partial Vector2 aether_input_mouse_delta();

    [LibraryImport(Lib)]
    internal static partial Vector2 aether_input_scroll_delta();

    // ── Entity & scene (Module 01) ──────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_entity_set_parent(uint child, uint parent);

    [LibraryImport(Lib)]
    internal static partial uint aether_entity_get_parent(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_entity_child_count(uint id);

    [LibraryImport(Lib)]
    internal static partial uint aether_entity_child_at(uint id, int index);

    [LibraryImport(Lib)]
    internal static partial void aether_entity_set_active(uint id, int active);

    [LibraryImport(Lib)]
    internal static partial int aether_entity_is_active(uint id);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_scene_find_by_name(string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_scene_create_entity(string name, Vector3 pos);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_scene_instantiate_prefab(string name, Vector3 pos);

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
    internal static partial uint aether_camera_create_orthographic(Vector3 pos, float height);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_main(uint id);

    [LibraryImport(Lib)]
    internal static partial uint aether_camera_get_main();

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_mode(uint id, int mode);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_perspective(uint id, float fovDeg);

    [LibraryImport(Lib)]
    internal static partial void aether_camera_set_orthographic(uint id, float height);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_camera_get_projection(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_camera_get_orthographic_height(uint id);

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

    [LibraryImport(Lib)]
    internal static partial void aether_physics_set_angular_velocity(uint id, Vector3 velocity);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_physics_get_angular_velocity(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_force(uint id, Vector3 force);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_impulse(uint id, Vector3 impulse);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_torque(uint id, Vector3 torque);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_add_angular_impulse(uint id, Vector3 impulse);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_freeze_rotation(uint id, int x, int y, int z);

    [LibraryImport(Lib)]
    internal static partial RaycastHit aether_physics_raycast(Vector3 origin, Vector3 direction, float maxDistance);

    [LibraryImport(Lib)]
    internal static partial RaycastHit aether_physics_spherecast(Vector3 origin, Vector3 direction, float radius, float maxDistance);

    [LibraryImport(Lib)]
    internal static partial int aether_physics_overlap_sphere(Vector3 center, float radius);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_physics_overlap_at(int index);

    [LibraryImport(Lib)]
    internal static partial void aether_physics_enable_events(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_physics_event_count(uint id, int kind);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_physics_event_at(uint id, int kind, int index);

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

    // ── World: behaviors / scripts / tags / iteration ─────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_add_bob(uint id, float amplitude, float frequency, float phase);

    [LibraryImport(Lib)]
    internal static partial void aether_add_spin(uint id, Vector3 eulerDegPerSec);

    [LibraryImport(Lib)]
    internal static partial void aether_add_orbit(uint id, Vector3 center, float radius, float speedDeg, float startAngleDeg, float yawOffsetDeg, float height);

    [LibraryImport(Lib)]
    internal static partial void aether_add_material_pulse(uint id, Vector3 emissiveA, Vector3 emissiveB, float frequency);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_add_script(uint id, string typeName);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_scene_file_exists(string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_tag_create(string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_tag_get_id(string name);

    [LibraryImport(Lib)]
    internal static partial void aether_tag_add(uint entityId, uint tagId);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_tag_has(uint entityId, uint tagId);

    [LibraryImport(Lib)]
    internal static partial void aether_tag_remove(uint entityId, uint tagId);

    [LibraryImport(Lib)]
    internal static partial int aether_world_get_entities_with_transform(uint* buf, int cap);

    [LibraryImport(Lib)]
    internal static partial int aether_tag_get_entities(uint tagId, uint* buf, int cap);

    // ── World: model / mesh / material ────────────────────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_load_model(uint id, string path);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_create_mesh(string type);

    [LibraryImport(Lib)]
    internal static partial void aether_add_mesh(uint entityId, uint meshHandle);

    [LibraryImport(Lib)]
    internal static partial void aether_set_material(uint id, Vector3 color, float metallic, float roughness);

    [LibraryImport(Lib)]
    internal static partial void aether_set_material_color(uint id, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial uint aether_make_material(Vector3 color, float metallic, float roughness);

    [LibraryImport(Lib)]
    internal static partial void aether_bind_material(uint entityId, uint materialId);

    [LibraryImport(Lib)]
    internal static partial void aether_material_set_color(uint materialId, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_material_set_metallic(uint materialId, float value);

    [LibraryImport(Lib)]
    internal static partial void aether_material_set_roughness(uint materialId, float value);

    [LibraryImport(Lib)]
    internal static partial void aether_material_set_emissive(uint materialId, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_entity_material_set_color(uint entityId, Vector3 color);

    [LibraryImport(Lib)]
    internal static partial void aether_entity_material_set_metallic(uint entityId, float value);

    [LibraryImport(Lib)]
    internal static partial void aether_entity_material_set_roughness(uint entityId, float value);

    [LibraryImport(Lib)]
    internal static partial void aether_entity_material_set_emissive(uint entityId, Vector3 color);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_set_material_texture(uint entityId, string path);

    // ── Animation ─────────────────────────────────────────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_anim_add(uint id, string animPath, int lockRoot);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_anim_load_external(uint id, string animPath);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_compile(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_clear_pending(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_set_clip(uint id, int clipIndex);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_anim_get_current(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_set_playback_speed(uint id, float speed);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_anim_get_playback_speed(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_set_time(uint id, float t);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_anim_get_time(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_anim_get_count(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_anim_get_name(uint id, int index, byte* buf, int bufLen);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_anim_get_duration(uint id);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_anim_find(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial int aether_anim_get_entities_with_animator(uint* buf, int cap);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_set_blend(uint id, int secondaryClipIndex, float transitionSpeed);

    [LibraryImport(Lib)]
    internal static partial void aether_anim_set_root_motion_enabled(uint id, int enabled);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_anim_get_root_motion_enabled(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_anim_get_root_motion_delta(uint id);
}
