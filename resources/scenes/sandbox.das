options gen2

require world
require renderer
require camera
require physics
require input
require ui
require imgui
require math
require animation
require systems/fox_system

// ── Scene state ───────────────────────────────────────────────────────────────

var g_fox_count    : int   = 0
var g_speed_scale  : float = 1.0
var g_rotate_foxes : bool  = true
var g_spawn_btn    : uint  = 0u
var g_speed_slider : uint  = 0u
var g_rotate_cb    : uint  = 0u
var g_fps_label    : uint  = 0u

var g_cube_mesh   : uint  = 0u
var g_sphere_mesh : uint  = 0u

// ── Day/Night cycle state ─────────────────────────────────────────────────────

var g_day_night_enabled : bool  = true
var g_time_speed        : float = 60.0   // simulated seconds per real second
var g_time_of_day       : float = 12.0   // hours (0-24)
var g_manual_mode       : bool  = false  // when true, sandbox controls lighting directly

var g_dn_toggle_btn     : uint  = 0u
var g_time_slider       : uint  = 0u
var g_speed_slider_dn   : uint  = 0u
var g_manual_cb         : uint  = 0u
var g_time_label        : uint  = 0u
var g_sun_dir_label     : uint  = 0u

// ── on_attach ─────────────────────────────────────────────────────────────────

[export]
def on_attach(world : World?)
{
	// Cache primitive mesh handles for reuse across entities.
	g_cube_mesh   = create_mesh(world, "cube")
	g_sphere_mesh = create_mesh(world, "sphere")

	// ── Lighting ──────────────────────────────────────────────────────────────
	set_ambient(float3(0.04, 0.05, 0.07))
	set_sun(float3(-0.5, -0.8, -0.3), 3.0, float3(1.0, 0.88, 0.72))
	add_point_light(float3( 10.0, 7.0,  4.0), float3(1.0, 0.60, 0.20), 6.0, 24.0, true)
	add_point_light(float3(-10.0, 7.0, -4.0), float3(0.20, 0.60, 1.0), 6.0, 24.0, true)
	add_spot_light(float3( 0.0, 8.0, -14.0), float3(0.95, 0.80, 0.55), 10.0, 28.0,
		float3( 0.0, -0.55,  0.83), 0.30, 0.55, true)
	add_spot_light(float3( 0.0, 8.0,  14.0), float3(0.95, 0.80, 0.55), 10.0, 28.0,
		float3( 0.0, -0.55, -0.83), 0.30, 0.55, true)
	add_spot_light(float3(-14.0, 8.0,  0.0), float3(0.95, 0.85, 0.60), 10.0, 28.0,
		float3( 0.83, -0.55,  0.0), 0.30, 0.55, true)
	add_spot_light(float3( 14.0, 8.0,  0.0), float3(0.95, 0.85, 0.60), 10.0, 28.0,
		float3(-0.83, -0.55,  0.0), 0.30, 0.55, true)
	add_point_light(float3( 5.0, 3.0,  5.0), float3(1.0, 0.25, 0.25), 3.0, 10.0, true)
	add_point_light(float3(-5.0, 3.0, -5.0), float3(0.25, 1.0, 0.25), 3.0, 10.0, true)
	set_sky(float3(0.45, 0.65, 1.0), float3(0.07, 0.14, 0.55))

	// ── Camera ────────────────────────────────────────────────────────────────
	let cam = create_orbit_camera(float3(0.0, 7.0, 18.0), float3(0.0, 0.5, 0.0), 55.0)
	set_main_camera(cam)

	// ── Wide ground platform ──────────────────────────────────────────────────
	let floor = entity_create(world)
	add_transform(world, floor)
	set_transform(world, floor,
		float3(0.0, -0.5, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(40.0, 1.0, 40.0))
	add_mesh(world, floor, g_cube_mesh)
	add_box_body(world, floor, float3(20.0, 0.5, 20.0), false)

	// ── Center box stack ──────────────────────────────────────────────────────
	for (row in range(4))
	{
		for (col in range(4))
		{
			let bx = entity_create(world)
			add_transform(world, bx)
			set_transform(world, bx,
				float3(float(col - 1) * 1.6, 1.0 + float(row) * 1.8, float(row - 1) * 1.6),
				float3(0.0, 0.0, 0.0),
				float3(0.7, 0.7, 0.7))
			add_mesh(world, bx, g_cube_mesh)
			add_box_body(world, bx, float3(0.35, 0.35, 0.35), true)
		}
	}

	// ── Tall pillars ──────────────────────────────────────────────────────────
	let pillar_positions = [
		float3( 4.0, 0.0,  4.0), float3(-4.0, 0.0,  4.0),
		float3( 4.0, 0.0, -4.0), float3(-4.0, 0.0, -4.0),
		float3( 6.0, 0.0,  0.0), float3(-6.0, 0.0,  0.0),
		float3( 0.0, 0.0,  6.0), float3( 0.0, 0.0, -6.0)
	]
	for (pos in pillar_positions)
	{
		let pillar = entity_create(world)
		add_transform(world, pillar)
		set_transform(world, pillar,
			float3(pos.x, 2.0, pos.z),
			float3(0.0, 0.0, 0.0),
			float3(0.5, 4.0, 0.5))
		add_mesh(world, pillar, g_cube_mesh)
		add_box_body(world, pillar, float3(0.25, 2.0, 0.25), false)
	}

	// ── Ground-level sphere grid ──────────────────────────────────────────────
	for (i in range(5))
	{
		for (j in range(5))
		{
			let sx = -8.0 + float(i) * 4.0
			let sz = -8.0 + float(j) * 4.0
			if (abs(sx) > 3.0 || abs(sz) > 3.0)
			{
				let s = entity_create(world)
				add_transform(world, s)
				set_transform(world, s,
					float3(sx, 0.4, sz),
					float3(0.0, 0.0, 0.0),
					float3(0.4, 0.4, 0.4))
				add_mesh(world, s, g_sphere_mesh)
				add_sphere_body(world, s, 0.4, false)
			}
		}
	}

	// ── Physics sphere ────────────────────────────────────────────────────────
	let sphere = entity_create(world)
	add_transform(world, sphere)
	set_transform(world, sphere,
		float3(4.0, 12.0, -3.0),
		float3(0.0, 0.0, 0.0),
		float3(0.7, 0.7, 0.7))
	add_mesh(world, sphere, g_sphere_mesh)
	add_sphere_body(world, sphere, 0.7, true)

	// ── Physics capsule ───────────────────────────────────────────────────────
	let capsule = entity_create(world)
	add_transform(world, capsule)
	set_transform(world, capsule,
		float3(-4.0, 14.0, 3.0),
		float3(0.0, 0.0, 0.0),
		float3(1.0, 1.0, 1.0))
	add_mesh(world, capsule, g_cube_mesh)
	add_capsule_body(world, capsule, 0.6, 0.6, true)

	// ── Foxes ─────────────────────────────────────────────────────────────────
	spawn_foxes(world, 3, 10.0)
	g_fox_count = 3

	// ── Native UI - Scene Controls panel ──────────────────────────────────────
	let panel = create_ui_panel(world, 16.0, 16.0, 280.0, 240.0, "Scene Controls")
	set_ui_panel_collapsible(world, panel, true)

	g_fps_label = create_ui_label(world, 16.0, 56.0, 260.0, 24.0, "FPS: --")
	set_ui_font_size(world, g_fps_label, 13.0)
	set_ui_text_color(world, g_fps_label, float4(0.7, 0.85, 1.0, 1.0))
	add_ui_child(world, panel, g_fps_label)

	let sep_lbl = create_ui_label(world, 16.0, 88.0, 260.0, 20.0, "Fox rotation speed")
	set_ui_font_size(world, sep_lbl, 12.0)
	set_ui_text_color(world, sep_lbl, float4(0.65, 0.65, 0.72, 1.0))
	add_ui_child(world, panel, sep_lbl)

	g_speed_slider = create_ui_slider(world, 16.0, 112.0, 260.0, 28.0, 0.1, 5.0, 1.0)
	add_ui_child(world, panel, g_speed_slider)

	g_rotate_cb = create_ui_checkbox(world, 16.0, 152.0, 260.0, 28.0, "Rotate foxes", true)
	add_ui_child(world, panel, g_rotate_cb)

	g_spawn_btn = create_ui_button(world, 16.0, 192.0, 260.0, 40.0, "Spawn Fox")
	set_ui_button_color(world, g_spawn_btn,       float4(0.15, 0.38, 0.20, 1.0))
	set_ui_button_hover_color(world, g_spawn_btn, float4(0.20, 0.50, 0.27, 1.0))
	set_ui_button_press_color(world, g_spawn_btn, float4(0.10, 0.28, 0.15, 1.0))
	add_ui_child(world, panel, g_spawn_btn)

	// ── Native UI - Day/Night Controls panel ──────────────────────────────────
	let dn_panel = create_ui_panel(world, 312.0, 16.0, 280.0, 260.0, "Day / Night")
	set_ui_panel_collapsible(world, dn_panel, true)

	g_time_label = create_ui_label(world, 328.0, 56.0, 260.0, 24.0, "Time: 12:00")
	set_ui_font_size(world, g_time_label, 14.0)
	set_ui_text_color(world, g_time_label, float4(1.0, 0.95, 0.70, 1.0))
	add_ui_child(world, dn_panel, g_time_label)

	g_sun_dir_label = create_ui_label(world, 328.0, 84.0, 260.0, 20.0, "Sun: (0.00, 1.00, 0.00)")
	set_ui_font_size(world, g_sun_dir_label, 11.0)
	set_ui_text_color(world, g_sun_dir_label, float4(0.65, 0.65, 0.72, 1.0))
	add_ui_child(world, dn_panel, g_sun_dir_label)

	let tod_lbl = create_ui_label(world, 328.0, 112.0, 260.0, 20.0, "Time of day (hours)")
	set_ui_font_size(world, tod_lbl, 12.0)
	set_ui_text_color(world, tod_lbl, float4(0.65, 0.65, 0.72, 1.0))
	add_ui_child(world, dn_panel, tod_lbl)

	g_time_slider = create_ui_slider(world, 328.0, 136.0, 260.0, 28.0, 0.0, 24.0, 12.0)
	add_ui_child(world, dn_panel, g_time_slider)

	let spd_lbl = create_ui_label(world, 328.0, 172.0, 260.0, 20.0, "Time speed (sec/sec)")
	set_ui_font_size(world, spd_lbl, 12.0)
	set_ui_text_color(world, spd_lbl, float4(0.65, 0.65, 0.72, 1.0))
	add_ui_child(world, dn_panel, spd_lbl)

	g_speed_slider_dn = create_ui_slider(world, 328.0, 196.0, 260.0, 28.0, 0.0, 300.0, 60.0)
	add_ui_child(world, dn_panel, g_speed_slider_dn)

	g_manual_cb = create_ui_checkbox(world, 328.0, 232.0, 260.0, 28.0, "Manual mode (pause auto)", false)
	add_ui_child(world, dn_panel, g_manual_cb)
}

// ── Helper: format time of day as HH:MM ───────────────────────────────────────

def format_time(hours : float) : string
{
	let h = int(hours) % 24
	let m = int((hours - float(int(hours))) * 60.0) % 60
	return "{h:02}:{m:02}"
}

// ── on_update ─────────────────────────────────────────────────────────────────

[export]
def on_update(world : World?)
{
	let dt = get_delta_time()

	// ── Read UI controls ──────────────────────────────────────────────────────
	g_speed_scale  = get_ui_slider_value(world, g_speed_slider)
	g_rotate_foxes = get_ui_checked(world, g_rotate_cb)

	// Spawn a new fox when button clicked
	if (get_ui_clicked(world, g_spawn_btn))
	{
		let angle  = float(g_fox_count) * 0.618 * 6.2832
		let radius = 3.0 + sqrt(float(g_fox_count)) * 1.8
		let pos    = float3(cos(angle) * radius, 0.0, sin(angle) * radius)
		let yaw    = angle * 57.2958 + 90.0

		let fox = entity_create(world)
		add_transform(world, fox)
		set_transform(world, fox, pos, float3(0.0, yaw, 0.0), float3(0.05, 0.05, 0.05))
		load_model(world, fox, "assets://models/Fox/Fox.mesh")
		set_animation(world, fox, 2)
		set_playback_speed(world, fox, 0.7)
		set_anim_time(world, fox, float(g_fox_count) * 0.3)
		g_fox_count++

		set_ui_button_label(world, g_spawn_btn, "Spawn Fox ({g_fox_count})")
	}

	// Rotate all foxes if enabled
	if (g_rotate_foxes)
	{
		for_each_with_transform(world) $(e : uint)
		{
			let scale = get_scale(world, e)
			if (scale.x < 0.1)
			{
				let pos    = get_position(world, e)
				let euler  = get_euler(world, e)
				let angle  = atan2(pos.x, pos.z)
				let r      = sqrt(pos.x * pos.x + pos.z * pos.z)
				if (r > 0.5)
				{
					let na  = angle + dt * g_speed_scale * 0.6
					let np  = float3(sin(na) * r, pos.y, cos(na) * r)
					let ny  = na * 57.2958 + 90.0
					set_transform(world, e, np, float3(euler.x, ny, euler.z), scale)
				}
			}
		}
	}

	// Switch fox animations
	let anim_clip = g_rotate_foxes ? 2 : 0
	for_each_with_animator(world) $(e : uint)
	{
		if (get_current_animation(world, e) != anim_clip)
		{
			set_animation(world, e, anim_clip)
			set_playback_speed(world, e, g_rotate_foxes ? 0.7 : 1.0)
		}
	}

	// ── Day/Night cycle controls ──────────────────────────────────────────────
	g_manual_mode = get_ui_checked(world, g_manual_cb)
	set_day_night_enabled(!g_manual_mode)

	if (g_manual_mode)
	{
		// Manual mode: sandbox directly sets time of day via slider
		let new_time = get_ui_slider_value(world, g_time_slider)
		if (abs(new_time - g_time_of_day) > 0.01)
		{
			g_time_of_day = new_time
			set_time_of_day(g_time_of_day)
		}
	}
	else
	{
		// Auto mode: read time speed from slider, read current time from system
		g_time_speed = get_ui_slider_value(world, g_speed_slider_dn)
		set_time_speed(g_time_speed)
		g_time_of_day = get_time_of_day()
		// Sync slider to current auto time
		set_ui_slider_value(world, g_time_slider, g_time_of_day)
	}

	// Update time display
	let time_str = format_time(g_time_of_day)
	set_ui_text(world, g_time_label, "Time: {time_str}")

	// Update sun direction display
	let sun_dir = get_sun_direction()
	let sun_str = "Sun: ({sun_dir.x:.2f}, {sun_dir.y:.2f}, {sun_dir.z:.2f})"
	set_ui_text(world, g_sun_dir_label, sun_str)

	// ── FPS label ─────────────────────────────────────────────────────────────
	let fps = imgui_get_frame_rate()
	set_ui_text(world, g_fps_label, "FPS: {int(fps)}")

	// ── ImGui debug overlay ───────────────────────────────────────────────────
	imgui_set_next_window_pos_once(16.0, 272.0)
	imgui_set_next_window_size_once(280.0, 0.0)
	if (imgui_begin("Debug Info"))
	{
		imgui_text("Scene")
		imgui_separator()
		imgui_label_text("Foxes", "{g_fox_count}")
		imgui_label_text("Speed", "{g_speed_scale}")
		imgui_label_text("Rotate", "{g_rotate_foxes}")
		imgui_spacing()
		imgui_separator()
		imgui_text_colored(float4(0.6, 0.9, 0.6, 1.0), "Lights")
		imgui_separator()
		imgui_label_text("Point lights", "6  (all shadow)")
		imgui_label_text("Spot lights", "4  (all shadow)")
		imgui_spacing()
		imgui_separator()
		imgui_text_colored(float4(1.0, 0.9, 0.5, 1.0), "Day / Night")
		imgui_separator()
		imgui_label_text("Time", "{time_str}")
		imgui_label_text("Mode", g_manual_mode ? "Manual" : "Auto")
		imgui_label_text("Speed", "{g_time_speed:.1f} sec/sec")
		imgui_spacing()
		imgui_separator()
		imgui_text_colored(float4(0.5, 0.9, 0.5, 1.0), "Hot-reload: F5")
	}
	imgui_end()

	// ── Keyboard shortcuts ────────────────────────────────────────────────────
	if (is_key_pressed(32))  // Space - toggle fox rotation
	{
		g_rotate_foxes = !g_rotate_foxes
		set_ui_checked(world, g_rotate_cb, g_rotate_foxes)
	}

	if (is_key_down(61))  // = / +
	{
		g_speed_scale = clamp(g_speed_scale + dt * 2.0, 0.1, 5.0)
		set_ui_slider_value(world, g_speed_slider, g_speed_scale)
	}
	if (is_key_down(45))  // -
	{
		g_speed_scale = clamp(g_speed_scale - dt * 2.0, 0.1, 5.0)
		set_ui_slider_value(world, g_speed_slider, g_speed_scale)
	}

	// T - toggle day/night manual mode
	if (is_key_pressed(84))
	{
		g_manual_mode = !g_manual_mode
		set_ui_checked(world, g_manual_cb, g_manual_mode)
		set_day_night_enabled(!g_manual_mode)
	}

	// Page Up / Page Down - scrub time forward/backward in manual mode
	if (is_key_down(73) && g_manual_mode)  // Page Up
	{
		g_time_of_day = clamp(g_time_of_day + dt * 2.0, 0.0, 24.0)
		if (g_time_of_day >= 24.0)
		{
			g_time_of_day = 0.0
		}
		set_time_of_day(g_time_of_day)
		set_ui_slider_value(world, g_time_slider, g_time_of_day)
	}
	if (is_key_down(74) && g_manual_mode)  // Page Down
	{
		g_time_of_day = g_time_of_day - dt * 2.0
		if (g_time_of_day < 0.0)
		{
			g_time_of_day = 24.0
		}
		set_time_of_day(g_time_of_day)
		set_ui_slider_value(world, g_time_slider, g_time_of_day)
	}

	// Home - jump to noon
	if (is_key_pressed(71))  // Home
	{
		g_time_of_day = 12.0
		set_time_of_day(12.0)
		set_ui_slider_value(world, g_time_slider, 12.0)
	}

	// End - jump to midnight
	if (is_key_pressed(79))  // End
	{
		g_time_of_day = 0.0
		set_time_of_day(0.0)
		set_ui_slider_value(world, g_time_slider, 0.0)
	}
}

// ── on_detach ─────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
}
