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

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_sprite_animator_set_animation(uint id, string path);
    [LibraryImport(Lib)] internal static partial void aether_sprite_animator_play(uint id);
    [LibraryImport(Lib)] internal static partial void aether_sprite_animator_pause(uint id);
    [LibraryImport(Lib)] internal static partial void aether_sprite_animator_restart(uint id);
    [LibraryImport(Lib)] internal static partial int aether_sprite_animator_is_playing(uint id);
    [LibraryImport(Lib)] internal static partial uint aether_sprite_animator_get_current_frame(uint id);
    [LibraryImport(Lib)] internal static partial float aether_sprite_animator_get_speed(uint id);
    [LibraryImport(Lib)] internal static partial void aether_sprite_animator_set_speed(uint id, float value);
    [LibraryImport(Lib)] internal static partial int aether_sprite_animator_get_loop_mode(uint id);
    [LibraryImport(Lib)] internal static partial void aether_sprite_animator_set_loop_mode(uint id, int value);
    [LibraryImport(Lib)] internal static unsafe partial int aether_sprite_animator_pop_event(uint id, byte* name, int nameCapacity, byte* payload, int payloadCapacity, uint* frameIndex);

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

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_font(uint id, string name);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_wrap(uint id, int wrap);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_text(uint id, byte* buf, int bufLen);

    // ── Assets (generic data-asset text read/write/list) ────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_assets_read_text(string vpath, byte* buf, int cap);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_assets_write_text(string vpath, string text);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_assets_list(string pattern, byte* buf, int cap);

    // ── EditorGui (editor-only ImGui bridge; resolves lazily, never called in a shipped game) ────
    [LibraryImport(Lib)] internal static partial void aether_editorgui_set_next_window_size(Vector2 size);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_begin(string title, int* open);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_end();
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_begin_child(string id, Vector2 size, int border);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_end_child();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_same_line();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_set_next_item_width(float w);
    [LibraryImport(Lib)] internal static partial float aether_editorgui_frame_height();
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial Vector2 aether_editorgui_calc_text_size(string s);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_push_clip_rect(Vector2 mn, Vector2 mx, int intersect);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_pop_clip_rect();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_separator();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_spacing();
    [LibraryImport(Lib)] internal static partial Vector2 aether_editorgui_content_avail();
    [LibraryImport(Lib)] internal static partial Vector2 aether_editorgui_cursor_screen_pos();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_set_cursor_screen_pos(Vector2 p);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_editorgui_text(string s);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_editorgui_text_colored(Vector4 col, string s);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_button(string label, Vector2 size);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_small_button(string label);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_checkbox(string label, int* v);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_input_text(string label, byte* buf, int bufLen);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_input_text_multiline(string label, byte* buf, int bufLen, Vector2 size);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_input_float(string label, float* v);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_selectable(string label, int selected);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_tree_node(string label);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_tree_pop();
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_combo(string label, int* current, string itemsNewlineJoined);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_begin_combo(string label, string preview);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_end_combo();
    [LibraryImport(Lib)] internal static partial void aether_editorgui_set_keyboard_focus_here(int offset);

    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_line(Vector2 a, Vector2 b, Vector4 col, float thick);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_rect_filled(Vector2 mn, Vector2 mx, Vector4 col, float rounding);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_rect(Vector2 mn, Vector2 mx, Vector4 col, float rounding, float thick);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_bezier(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4, Vector4 col, float thick);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_circle_filled(Vector2 c, float r, Vector4 col);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_triangle_filled(Vector2 a, Vector2 b, Vector2 c, Vector4 col);
    [LibraryImport(Lib)] internal static partial Vector4 aether_editorgui_theme_color(int id);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_editorgui_add_text(Vector2 p, Vector4 col, string s);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_invisible_button(string id, Vector2 size);
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_item_active();
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_item_hovered();
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_item_clicked();
    [LibraryImport(Lib)] internal static partial Vector2 aether_editorgui_mouse_pos();
    [LibraryImport(Lib)] internal static partial Vector2 aether_editorgui_mouse_drag_delta();
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_mouse_dragging();
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_mouse_clicked();
    [LibraryImport(Lib)] internal static partial int aether_editorgui_is_mouse_down();
    [LibraryImport(Lib)] internal static partial float aether_editorgui_mouse_wheel();

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

    [LibraryImport(Lib)]
    internal static partial int aether_ui_is_focused(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_activated(uint id);

    // The element holding the keyboard, or 0. Committed before any script runs, so every
    // script sees the same answer whatever order they update in.
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_ui_focused_entity();

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_focus(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_clear_focus();

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_interactable(uint id, int value);

    // ── Widgets ──────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial float aether_ui_get_slider_value(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_slider_value(uint id, float value);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_toggle(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_toggle(uint id, int on);

    [LibraryImport(Lib)]
    internal static partial float aether_ui_get_progress(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_progress(uint id, float value);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_button_label(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_button_label(uint id, string text);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_changed(uint id);

    // ── Text box ─────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial uint aether_ui_create_text_box(uint canvasId);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_text_box_text(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_text_box_text(uint id, string text);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_text_box_placeholder(uint id, string text);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_box_content_type(uint id, int contentType);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_box_max_length(uint id, int maxLength);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_submitted(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_cancelled(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_is_editing(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_begin_edit(uint id);

    // ── Custom-shader effects ────────────────────────────────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_ui_create_effect(uint canvas, string shader);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_effect_params(uint id, Vector4 param);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_effect_colors(uint id, Vector4 color0, Vector4 color1);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_effect_sort_order(uint id, int sortOrder);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_material(uint id, string shader);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_material_params(uint id, Vector4 param);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_material_colors(uint id, Vector4 color0, Vector4 color1);

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
    // ── Project custom render passes ───────────────────────────────────────────────
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_custompass_register(string name, string shader, int stage);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_custompass_unregister(string name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_custompass_submit(string name, Vector4* data, int count, Vector4 param, Vector4 color0, Vector4 color1);

    // ── Transient 2D lights / shadow occluders ─────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_light2d_submit_light(float x, float y, float radius, Vector3 color, float intensity, int castsShadow);

    [LibraryImport(Lib)]
    internal static partial void aether_light2d_submit_occluder_capsule(float ax, float ay, float bx, float by, float radius);

    [LibraryImport(Lib)]
    internal static partial float aether_time_total();

    [LibraryImport(Lib)]
    internal static partial float aether_time_unscaled();

    [LibraryImport(Lib)]
    internal static partial float aether_time_delta();

    [LibraryImport(Lib)]
    internal static partial long aether_time_frame_count();

    [LibraryImport(Lib)]
    internal static partial void aether_time_set_scale(float scale);

    [LibraryImport(Lib)]
    internal static partial float aether_time_get_scale();

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

    [LibraryImport(Lib)]
    internal static partial void aether_input_set_os_cursor_visible(int visible);

    [LibraryImport(Lib)]
    internal static partial void aether_cursor_set_visible(int visible);

    [LibraryImport(Lib)]
    internal static partial int aether_cursor_get_visible();

    [LibraryImport(Lib)]
    internal static partial int aether_cursor_enabled();

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_cursor_set_look(string texture, float hotspotX, float hotspotY, float size, int pixelArt);

    [LibraryImport(Lib)]
    internal static partial void aether_cursor_reset();

    [LibraryImport(Lib)]
    internal static partial int aether_input_get_os_cursor_visible();

    // ── Clipboard ────────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial int aether_input_get_clipboard(byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_input_set_clipboard(string text);

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
    internal static partial void aether_scene_load(string name);

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
    internal static partial void aether_set_scale(uint id, Vector3 scale);

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

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector3 aether_camera_screen_to_world(Vector2 screenPos);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector2 aether_camera_world_to_screen(Vector3 worldPos);

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

    // ── Physics 2D ────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_box(uint id, Vector2 size, int bodyType);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_circle(uint id, float radius, int bodyType);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_capsule(uint id, float radius, float height, int bodyType);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_set_trigger(uint id, int trigger);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_set_linear_velocity(uint id, Vector2 velocity);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial Vector2 aether_physics2d_get_linear_velocity(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_set_angular_velocity(uint id, float radiansPerSec);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial float aether_physics2d_get_angular_velocity(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_force(uint id, Vector2 force);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_impulse(uint id, Vector2 impulse);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_torque(uint id, float torque);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_add_angular_impulse(uint id, float impulse);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_set_gravity_scale(uint id, float scale);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_set_drop_through(uint id, float seconds);

    [LibraryImport(Lib)]
    internal static partial RaycastHit2D aether_physics2d_raycast(Vector2 origin, Vector2 direction, float maxDistance);

    [LibraryImport(Lib)]
    internal static partial RaycastHit2D aether_physics2d_circlecast(Vector2 origin, float radius, Vector2 direction, float maxDistance);

    [LibraryImport(Lib)]
    internal static partial int aether_physics2d_is_point_solid(Vector2 point);

    [LibraryImport(Lib)]
    internal static partial int aether_physics2d_overlap_circle(Vector2 center, float radius);

    [LibraryImport(Lib)]
    internal static partial int aether_physics2d_overlap_point(Vector2 point);

    [LibraryImport(Lib)]
    internal static partial int aether_physics2d_overlap_aabb(Vector2 min, Vector2 max);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_physics2d_overlap_at(int index);

    [LibraryImport(Lib)]
    internal static partial void aether_physics2d_enable_events(uint id);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_physics2d_event_count(uint id, int kind);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_physics2d_event_at(uint id, int kind, int index);

    // ── Particles ─────────────────────────────────────────────────────────────
    [LibraryImport(Lib)]
    internal static partial void aether_particles_burst(uint id, int count);

    [LibraryImport(Lib)]
    internal static partial void aether_particles_set_emitting(uint id, int on);

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
    // ── Reflected components ──────────────────────────────────────────────────
    // Generic access to any AE_COMPONENT field by name, over the same reflection table
    // the inspector and scene serializer use. See ComponentExports.cpp.
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_has(uint id, string type);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_add(uint id, string type);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_remove(uint id, string type);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_get_number(uint id, string type, string field, out double value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_set_number(uint id, string type, string field, double value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_get_vector(uint id, string type, string field, out Vector4 value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_set_vector(uint id, string type, string field, Vector4 value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial int aether_component_get_string(uint id, string type, string field, byte* buffer, int capacity);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_component_set_string(uint id, string type, string field, string value);

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

    // ── Networking: RPCs ─────────────────────────────────────────────────────────
    // Runs a [NetRpc] method declared on a script attached to `entityId`, routed by
    // the target the attribute declared. `expectedTarget` is -1 for "whatever the
    // method declares" or a NetRpcTarget value the declaration must match. Returns 0
    // when the call is refused - no such RPC, a target mismatch, a client trying to
    // originate a host-to-client call, or an unreplicated entity that has no name on
    // the wire (see Net.Call). argBlob/argLen carry the single argument shape RPCs
    // support today: null/0 for none, otherwise a UTF-8 string blob.
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial int aether_net_call_rpc(uint entityId, string methodName, byte* argBlob, int argLen,
        int expectedTarget);

    // ── Networking: session ──────────────────────────────────────────────────────
    // Every one of these is safe with no session and no NetworkContext registered:
    // they report 0/false rather than failing, so a title screen can ask before
    // anything has connected.
    // `maxConnections` caps simultaneous CLIENT connections (the host is not one of
    // them). A joiner past the cap is refused with a reason it can read back through
    // aether_net_disconnect_reason, not dropped silently.
    [LibraryImport(Lib)]
    internal static partial int aether_net_host(ushort port, int maxConnections);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_net_connect(string host, ushort port);

    [LibraryImport(Lib)]
    internal static partial void aether_net_disconnect();

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_is_host();

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_is_client();

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_is_connected();

    // "This peer is (not) standing in the scene the session's entities belong to."
    // Setting it true is an arrival: it discards whatever the session left in the world
    // this peer has left, re-derives the scene-placed net ids against the scene it is in
    // now, and asks the host to send the world again. Not SuppressGCTransition - the
    // setter touches the world and the transport.
    [LibraryImport(Lib)]
    internal static partial void aether_net_set_replication_ready(int ready);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_is_replication_ready();

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_net_local_connection_id();

    // Host only: copies up to `capacity` joined connection ids into `buffer` and
    // returns how many were written. A null buffer (or a non-positive capacity) is a
    // size query returning the total, so the managed side never guesses a peer cap.
    [LibraryImport(Lib)]
    internal static unsafe partial int aether_net_connections(uint* buffer, int capacity);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial uint aether_net_spawn(string prefab, Vector3 position, uint owner);

    [LibraryImport(Lib)]
    internal static partial void aether_net_despawn(uint entityId);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_has_authority(uint entityId);

    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial int aether_net_is_owner(uint entityId);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_net_set_player_name(uint entityId, string name);

    [LibraryImport(Lib)]
    internal static unsafe partial int aether_net_get_player_name(uint entityId, byte* buffer, int capacity);

    // Replicated round-trip time for one player, in ms, as measured by that player's
    // own peer. 0 for the host's player and offline.
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_net_get_player_ping(uint entityId);

    // This peer's own cost to reach the host, in ms. 0 on a host and offline.
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_net_round_trip_ms();

    // The connection owning an entity; this peer's own connection for an unowned or
    // unreplicated one.
    [LibraryImport(Lib)]
    [SuppressGCTransition]
    internal static partial uint aether_net_owner_of(uint entityId);

    // Every entity carrying a Net Player component. Two-call size query, like
    // aether_net_connections: pass a null buffer for the count.
    [LibraryImport(Lib)]
    internal static unsafe partial int aether_net_players(uint* buffer, int capacity);

    // Resolves `desired` against the names already taken by players ahead of this one
    // in the join order, writes the result onto the entity, and copies it back out.
    // Refused (returns 0, writes nothing) for an entity this peer does not own.
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial int aether_net_claim_player_name(uint entityId, string desired, byte* buffer,
        int capacity);

    // Why the last link ended, when the far end ended it deliberately and said so
    // (a refusal, or a host closing the session). Empty for an accidental drop.
    [LibraryImport(Lib)]
    internal static unsafe partial int aether_net_disconnect_reason(byte* buffer, int capacity);

    [LibraryImport(Lib)]
    internal static unsafe partial int aether_net_last_error(byte* buffer, int capacity);

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
