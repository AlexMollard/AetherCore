options gen2

require world
require renderer
require camera
require physics
require input
require ui
require imgui
require math
require systems/fox_system

// ── Scene state ───────────────────────────────────────────────────────────────

var g_fox_count    : int   = 0
var g_speed_scale  : float = 1.0
var g_rotate_foxes : bool  = true
var g_spawn_btn    : uint  = 0u
var g_speed_slider : uint  = 0u
var g_rotate_cb    : uint  = 0u
var g_fps_label    : uint  = 0u

// ── on_attach ─────────────────────────────────────────────────────────────────

[export]
def on_attach(world : World?)
{
	// ── Lighting ──────────────────────────────────────────────────────────────
	set_ambient(float3(0.06, 0.07, 0.10))
	set_sun(float3(-0.4, -1.0, -0.5), 2.2, float3(1.0, 0.96, 0.88))
	add_point_light(float3( 6.0, 4.0,  0.0), float3(1.0, 0.55, 0.15), 6.0, 20.0)
	add_point_light(float3(-6.0, 4.0,  0.0), float3(0.15, 0.55, 1.0), 4.0, 16.0)
	set_sky(float3(0.45, 0.65, 1.0), float3(0.07, 0.14, 0.55))

	// ── Camera ────────────────────────────────────────────────────────────────
	let cam = create_orbit_camera(float3(0.0, 6.0, 14.0), float3(0.0, 0.5, 0.0), 60.0)
	set_main_camera(cam)

	// ── Static floor ──────────────────────────────────────────────────────────
	let floor = entity_create(world)
	add_transform(world, floor)
	set_transform(world, floor,
		float3(0.0, -0.5, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(24.0, 1.0, 24.0))
	add_box_body(world, floor, float3(12.0, 0.5, 12.0), false)

	// ── Physics stack - boxes drop onto the floor ─────────────────────────────
	for (row in range(3))
	{
		for (col in range(3))
		{
			let box = entity_create(world)
			add_transform(world, box)
			set_transform(world, box,
				float3(float(col - 1) * 2.2, 3.0 + float(row) * 2.4, float(row - 1) * 2.2),
				float3(0.0, 0.0, 0.0),
				float3(1.0, 1.0, 1.0))
			add_box_body(world, box, float3(0.5, 0.5, 0.5), true)
		}
	}

	// ── Physics sphere ────────────────────────────────────────────────────────
	let sphere = entity_create(world)
	add_transform(world, sphere)
	set_transform(world, sphere,
		float3(4.0, 8.0, -2.0),
		float3(0.0, 0.0, 0.0),
		float3(1.0, 1.0, 1.0))
	add_sphere_body(world, sphere, 0.6, true)

	// ── Physics capsule ───────────────────────────────────────────────────────
	let capsule = entity_create(world)
	add_transform(world, capsule)
	set_transform(world, capsule,
		float3(-4.0, 10.0, 2.0),
		float3(0.0, 0.0, 0.0),
		float3(1.0, 1.0, 1.0))
	add_capsule_body(world, capsule, 0.6, 0.3, true)

	// ── Foxes in a circle ─────────────────────────────────────────────────────
	spawn_foxes(world, 12, 8.0)
	g_fox_count = 12

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
}

// ── on_update ─────────────────────────────────────────────────────────────────

[export]
def on_update(world : World?)
{
	let dt = get_delta_time()

	// Read UI controls
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
		g_fox_count++

		set_ui_button_label(world, g_spawn_btn, "Spawn Fox ({g_fox_count})")
	}

	// Rotate all foxes if enabled
	if (g_rotate_foxes)
	{
		for_each_with_transform(world) $(e : uint)
		{
			let scale = get_scale(world, e)
			if (scale.x < 0.1)  // only fox-scale entities (0.05)
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

	// Update FPS label
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
		imgui_text_colored(float4(0.5, 0.9, 0.5, 1.0), "Hot-reload: F5")
	}
	imgui_end()

	// ── Keyboard shortcuts ────────────────────────────────────────────────────
	if (is_key_pressed(32))  // Space - toggle rotation
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
}

// ── on_detach ─────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
}
