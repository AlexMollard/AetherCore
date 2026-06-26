options gen2;

require world;
require renderer;
require camera;
require physics;
require effects;
require input;
require keycodes;
require input_bindings;
require ui;
require math;
require animation;
require systems/fox_system;
require systems/character_controller;
require systems/third_person_camera;

// ── Scene state ───────────────────────────────────────────────────────────────

var g_fox_count    : int   = 0;
var g_speed_scale  : float = 1.0;
var g_rotate_foxes : bool  = true;
var g_spawn_btn    : uint  = 0u;
var g_speed_slider : uint  = 0u;
var g_rotate_cb    : uint  = 0u;
var g_cube_mesh   : uint  = 0u;
var g_sphere_mesh : uint  = 0u;

// ── Moving light state ─────────────────────────────────────────────────────────

var g_time           : float = 0.0;
var g_orbit_radius   : float = 20.0;
var g_orbit_height   : float = 3.0;

// ── Day/Night cycle state ─────────────────────────────────────────────────────

var g_day_night_enabled : bool  = true;
var g_time_speed        : float = 60.0;
var g_time_of_day       : float = 12.0;
var g_manual_mode       : bool  = false;

var g_dn_toggle_btn     : uint  = 0u;
var g_time_slider       : uint  = 0u;
var g_speed_slider_dn   : uint  = 0u;
var g_manual_cb         : uint  = 0u;
var g_time_label        : uint  = 0u;
var g_sun_dir_label     : uint  = 0u;

var g_plasma_sphere : uint = 0u;
var g_player : uint = 0u;
var g_player_idle_clip : int = 0;
var g_player_walk_clip : int = -1; // -1 means not yet added
var g_cam : uint = 0u;

// ── Tags ─────────────────────────────────────────────────────────────────────

var g_fox_tag : uint = 0u;
var g_ground_tag : uint = 0u;
var g_center_tag : uint = 0u;
var g_phys_toy_tag : uint = 0u;

// ── Entity inspector state ─────────────────────────────────────────────────────
//
// Selects a fox-tagged entity the user wants to push around with sliders. The
// inspector keeps the entity's last-applied transform so it can detect when a
// slider was user-dragged (its value drifted from the cached value), and only
// writes back to the world in that case so we never fight the server.

var g_ins_idx      : int  = -1;
var g_ins_entity    : uint = 0u;
var g_ins_count     : int  = 0;
var g_ins_x         : float = 0.0;
var g_ins_y         : float = 0.0;
var g_ins_z         : float = 0.0;
var g_ins_yaw       : float = 0.0;
var g_ins_sc        : float = 1.0;

var g_ins_label_idx : uint = 0u;
var g_ins_prev_btn  : uint = 0u;
var g_ins_next_btn  : uint = 0u;
var g_ins_x_sl      : uint = 0u;
var g_ins_y_sl      : uint = 0u;
var g_ins_z_sl      : uint = 0u;
var g_ins_yaw_sl    : uint = 0u;
var g_ins_sc_sl     : uint = 0u;

// Shared enumeration scratch (avoids needing array mutation inside blocks).
var g_enum_count  : int  = 0;
var g_enum_target : int  = -1;
var g_enum_result : uint = 0u;

// ── Physics toy / spawner state ────────────────────────────────────────────────

var g_toy_sz_sl    : uint = 0u;
var g_toy_h_sl     : uint = 0u;
var g_btn_cube     : uint = 0u;
var g_btn_sphere   : uint = 0u;
var g_btn_tower    : uint = 0u;
var g_btn_rain     : uint = 0u;
var g_btn_bounce   : uint = 0u;
var g_btn_clear    : uint = 0u;

// ── Helpers ────────────────────────────────────────────────────────────────────

// Count entities carrying the given tag, using only primitive global mutation.
def count_tagged(world : World?; tag : uint) : int
{
	g_enum_count = 0;
	for_each_with_tag(world, tag) $(e : uint)
	{
		g_enum_count += 1;
	};
	return g_enum_count;
}

// Find the Nth (0-based) entity with the given tag, using global scratch so
// the block only mutates simple scalars.
def find_nth_tagged(world : World?; tag : uint; n : int) : uint
{
	g_enum_count = 0;
	g_enum_target = n;
	g_enum_result = 0u;
	for_each_with_tag(world, tag) $(e : uint)
	{
		if (g_enum_count == g_enum_target) { g_enum_result = e; }
		g_enum_count += 1;
	};
	return g_enum_result;
}

// Re-sync the inspector sliders + label with the currently selected entity.
def refresh_inspector(world : World?)
{
	g_ins_count = count_tagged(world, g_fox_tag);
	if (g_ins_idx < 0 || g_ins_idx >= g_ins_count)
	{
		g_ins_entity = 0u;
		set_ui_text(world, g_ins_label_idx, "Selected: <none>");
		return;
	}
	g_ins_entity = find_nth_tagged(world, g_fox_tag, g_ins_idx);
	if (g_ins_entity == 0u)
	{
		set_ui_text(world, g_ins_label_idx, "Selected: <none>");
		return;
	}

	let pos = get_position(world, g_ins_entity);
	let eul = get_euler(world, g_ins_entity);
	let sc  = get_scale(world, g_ins_entity);
	g_ins_x   = pos.x;
	g_ins_y   = pos.y;
	g_ins_z   = pos.z;
	g_ins_yaw = eul.y;
	g_ins_sc  = sc.x; // foxes use uniform scale

	set_ui_slider_value(world, g_ins_x_sl,   g_ins_x);
	set_ui_slider_value(world, g_ins_y_sl,   g_ins_y);
	set_ui_slider_value(world, g_ins_z_sl,   g_ins_z);
	set_ui_slider_value(world, g_ins_yaw_sl, g_ins_yaw);
	set_ui_slider_value(world, g_ins_sc_sl,  g_ins_sc);
	let total = g_ins_count - 1;
	set_ui_text(world, g_ins_label_idx, "Selected: fox {g_ins_idx}/{total}  (id {g_ins_entity})");
}

// Spawn a single dynamic primitive with a transform and tag it as a physics toy.
// is_sphere == false -> box; is_sphere == true -> sphere.
def spawn_toy(world : World?; is_sphere : bool; pos : float3; size : float)
{
	let e = entity_create(world);
	add_transform(world, e);
	set_transform(world, e, pos, float3(0.0, 0.0, 0.0), float3(size, size, size));
	if (is_sphere)
	{
		add_mesh(world, e, g_sphere_mesh);
		add_sphere_body(world, e, size * 0.5, true);
	}
	else
	{
		add_mesh(world, e, g_cube_mesh);
		add_box_body(world, e, float3(size * 0.5, size * 0.5, size * 0.5), true);
	}
	add_tag(world, e, g_phys_toy_tag);
}

// Destroy every entity tagged as a physics toy. We destroy from highest index
// downward so that already-destroyed entities cannot be re-resolved.
def clear_toys(world : World?)
{
	let n = count_tagged(world, g_phys_toy_tag);
	var k = n;
	while (k > 0)
	{
		k -= 1;
		let e = find_nth_tagged(world, g_phys_toy_tag, k);
		if (e != 0u && entity_valid(e))
		{
			entity_destroy(world, e);
		}
	}
}

// Stack a vertical tower of dynamic boxes at world origin.
def spawn_tower(world : World?)
{
	let sz = get_ui_slider_value(world, g_toy_sz_sl);
	for (i in 0..5)
	{
		let y = 0.5 + float(i) * sz * 1.05;
		// tiny horizontal jitter so it teeters
		let jx = (float(i % 2) - 0.5) * 0.04;
		spawn_toy(world, false, float3(jx, y, 0.0), sz);
	}
}

// Scatter 4 dynamic spheres above the scene with random-ish offsets.
def spawn_rain(world : World?)
{
	let sz = get_ui_slider_value(world, g_toy_sz_sl);
	let h  = get_ui_slider_value(world, g_toy_h_sl);
	for (i in 0..4)
	{
		let ang = float(i) * 1.2566; // 72 deg spacing
		let x   = cos(ang) * 4.0 + float(i) * 0.7;
		let z   = sin(ang) * 4.0;
		spawn_toy(world, true, float3(x, h, z), sz);
	}
}

// ── on_attach ─────────────────────────────────────────────────────────────────

[export]
def on_attach(world : World?)
{
	g_cube_mesh   = create_mesh(world, "cube");
	g_sphere_mesh = create_mesh(world, "sphere");
	g_time        = 0.0;

	// ── Create tags ─────────────────────────────────────────────────────────
	g_fox_tag       = create_tag("fox");
	g_ground_tag    = create_tag("ground");
	g_center_tag    = create_tag("center");
	g_phys_toy_tag  = create_tag("phys_toy");

	// ── Lighting: sparse, well-separated for clear shadow testing ──────────
	clear_lights();
	set_ambient(float3(0.06, 0.06, 0.08));

	// Single directional sun - high angle, moderate intensity
	set_sun(float3(-0.3, -0.9, -0.2), 2.0, float3(1.0, 0.92, 0.78));

	// One warm point light - far left, elevated, illuminates left zone
	add_point_light(float3(-18.0, 2.0, 0.0), float3(1.0, 0.55, 0.15), 10.0, 30.0, true);

	// One cool point light - far right, elevated, illuminates right zone
	add_point_light(float3(18.0, 2.0, 0.0), float3(0.15, 0.45, 1.0), 10.0, 30.0, true);

	// One spot light - center, pointing down at the middle area
	add_spot_light(float3(-6.0, 2.5, 0.0), float3(1.0, 0.90, 0.70), 12.0, 25.0, float3(0.8, -1.0, 0.0), 0.20, 0.90, true);

	// Moving point light (index 2) - orbits the scene
	add_point_light(float3(g_orbit_radius, g_orbit_height, 0.0), float3(1.0, 0.2, 0.8), 8.0, 25.0, true);

	set_sky(float3(0.40, 0.60, 0.95), float3(0.08, 0.15, 0.50));

	// ── Camera ────────────────────────────────────────────────────────────────
	g_cam = create_orbit_camera(float3(0.0, 10.0, 15.0), float3(0.0, 1.0, 0.0), 50.0);
	set_main_camera(g_cam);

	// ── Wide ground platform ──────────────────────────────────────────────────
	let floor = entity_create(world);
	add_transform(world, floor);
	set_transform(world, floor,
		float3(0.0, -0.5, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(60.0, 1.0, 60.0));
	add_mesh(world, floor, g_cube_mesh);
	add_box_body(world, floor, float3(30.0, 0.5, 30.0), false);
	add_tag(world, floor, g_ground_tag);

	// ── LEFT ZONE: box stack under warm point light ─────────────────────────
	for (row in 0..3)
	{
		for (col in 0..3)
		{
			let bx = entity_create(world);
			add_transform(world, bx);
			set_transform(world, bx,
				float3(-18.0 + float(col - 1) * 1.8, 0.5 + float(row) * 1.6, float(row - 1) * 1.8),
				float3(0.0, 0.0, 0.0),
				float3(0.8, 0.8, 0.8));
			add_mesh(world, bx, g_cube_mesh);
			add_box_body(world, bx, float3(0.4, 0.4, 0.4), true);
		}
	}

	// ── RIGHT ZONE: sphere grid under cool point light ──────────────────────
	for (i in 0..4)
	{
		for (j in 0..4)
		{
			let s = entity_create(world);
			add_transform(world, s);
			set_transform(world, s,
				float3(14.0 + float(i) * 2.5, 0.5, -3.0 + float(j) * 2.5),
				float3(0.0, 0.0, 0.0),
				float3(0.5, 0.5, 0.5));
			add_mesh(world, s, g_sphere_mesh);
			add_sphere_body(world, s, 0.5, true);
		}
	}

	// ── CENTER ZONE: tall pillar under spot light ──────────────────────────
	let pillar = entity_create(world);
	add_transform(world, pillar);
	set_transform(world, pillar,
		float3(0.0, 3.0, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(0.6, 6.0, 0.6));
	add_mesh(world, pillar, g_cube_mesh);
	add_box_body(world, pillar, float3(0.3, 3.0, 0.3), false);
	add_tag(world, pillar, g_center_tag);

	// Small boxes around the center pillar to show spot shadow spread
	for (k in 0..6)
	{
		let angle = float(k) * 1.0472;  // 60 deg spacing
		let r = 4.0;
		let cb = entity_create(world);
		add_transform(world, cb);
		set_transform(world, cb,
			float3(cos(angle) * r, 0.4, sin(angle) * r),
			float3(0.0, 0.0, 0.0),
			float3(0.5, 0.5, 0.5));
		add_mesh(world, cb, g_cube_mesh);
		add_box_body(world, cb, float3(0.25, 0.25, 0.25), true);
	}

	// ── Plasma sphere (floating center, animated shader) ──────────────────
	g_plasma_sphere = entity_create(world);
	add_transform(world, g_plasma_sphere);
	set_transform(world, g_plasma_sphere,
		float3(0.0, 6.0, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(1.2, 1.2, 1.2));
	add_mesh(world, g_plasma_sphere, g_sphere_mesh);
	set_entity_effect(world, g_plasma_sphere, "plasma");
	set_effect_color(world, g_plasma_sphere, 1.0, 0.4, 0.9);
	set_effect_speed(world, g_plasma_sphere, 0.8);
	add_tag(world, g_plasma_sphere, g_center_tag);

	// ── Physics sphere (drops from above, left zone) ───────────────────────
	let sphere = entity_create(world);
	add_transform(world, sphere);
	set_transform(world, sphere,
		float3(-14.0, 14.0, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(0.8, 0.8, 0.8));
	add_mesh(world, sphere, g_sphere_mesh);
	add_sphere_body(world, sphere, 0.8, true);

	// ── Physics box (drops from above, right zone) ──────────────────────────
	let capsule = entity_create(world);
	add_transform(world, capsule);
	set_transform(world, capsule,
		float3(14.0, 16.0, 0.0),
		float3(0.0, 0.0, 0.0),
		float3(0.8, 0.8, 0.8));
	add_mesh(world, capsule, g_cube_mesh);
	add_box_body(world, capsule, float3(0.4, 0.4, 0.4), true);

	// ── Foxes in a circle (center) ──────────────────────────────────────────
	spawn_foxes(world, 3, 8.0, g_fox_tag);
	g_fox_count = 3;

	// ── Native UI - Scene Controls panel ────────────────────────────────────
	let panel = create_ui_panel(world, 16.0, 16.0, 280.0, 240.0, "Scene Controls");
	set_ui_panel_collapsible(world, panel, true);

	g_speed_slider = create_ui_slider(world, 16.0, 108.0, 260.0, 28.0, 0.1, 5.0, 1.0);
	set_ui_slider_label(world, g_speed_slider, "Fox rotation speed");
	add_ui_child(world, panel, g_speed_slider);

	g_rotate_cb = create_ui_checkbox(world, 16.0, 152.0, 260.0, 28.0, "Rotate foxes", true);
	add_ui_child(world, panel, g_rotate_cb);

	g_spawn_btn = create_ui_button(world, 16.0, 192.0, 260.0, 40.0, "Spawn Fox");
	set_ui_button_color(world, g_spawn_btn,       float4(0.23, 0.36, 0.19, 1.0));
	set_ui_button_hover_color(world, g_spawn_btn, float4(0.31, 0.46, 0.24, 1.0));
	set_ui_button_press_color(world, g_spawn_btn, float4(0.17, 0.27, 0.14, 1.0));
	add_ui_child(world, panel, g_spawn_btn);

	// ── Native UI - Day/Night Controls panel ────────────────────────────────
	let dn_panel = create_ui_panel(world, 312.0, 16.0, 280.0, 216.0, "Day / Night");
	set_ui_panel_collapsible(world, dn_panel, true);

	g_time_label = create_ui_label(world, 328.0, 44.0, 260.0, 22.0, "Time: 12:00");
	set_ui_font_size(world, g_time_label, 14.0);
	set_ui_text_color(world, g_time_label, float4(0.98, 0.76, 0.38, 1.0));
	add_ui_child(world, dn_panel, g_time_label);

	g_sun_dir_label = create_ui_label(world, 328.0, 70.0, 260.0, 20.0, "Sun: (0.00, 1.00, 0.00)");
	set_ui_font_size(world, g_sun_dir_label, 11.0);
	set_ui_text_color(world, g_sun_dir_label, float4(0.60, 0.66, 0.62, 1.0));
	add_ui_child(world, dn_panel, g_sun_dir_label);

	g_time_slider = create_ui_slider(world, 328.0, 108.0, 260.0, 28.0, 0.0, 24.0, 12.0);
	set_ui_slider_label(world, g_time_slider, "Time of day (hours)");
	add_ui_child(world, dn_panel, g_time_slider);

	g_speed_slider_dn = create_ui_slider(world, 328.0, 156.0, 260.0, 28.0, 0.0, 300.0, 60.0);
	set_ui_slider_label(world, g_speed_slider_dn, "Time speed (sec/sec)");
	add_ui_child(world, dn_panel, g_speed_slider_dn);

	g_manual_cb = create_ui_checkbox(world, 328.0, 188.0, 260.0, 28.0, "Manual mode (pause auto)", false);
	add_ui_child(world, dn_panel, g_manual_cb);

	// ── NEW Native UI - Entity Inspector panel ──────────────────────────────
	let ins_panel = create_ui_panel(world, 16.0, 280.0, 280.0, 376.0, "Entity Inspector");
	set_ui_panel_collapsible(world, ins_panel, true);

	g_ins_label_idx = create_ui_label(world, 32.0, 320.0, 260.0, 24.0, "Selected: <none>");
	set_ui_font_size(world, g_ins_label_idx, 13.0);
	set_ui_text_color(world, g_ins_label_idx, float4(0.95, 0.77, 0.42, 1.0));
	add_ui_child(world, ins_panel, g_ins_label_idx);

	g_ins_prev_btn = create_ui_button(world, 32.0, 352.0, 124.0, 30.0, "Prev Fox");
	set_ui_button_color(world, g_ins_prev_btn, float4(0.16, 0.20, 0.21, 1.0));
	add_ui_child(world, ins_panel, g_ins_prev_btn);

	g_ins_next_btn = create_ui_button(world, 162.0, 352.0, 124.0, 30.0, "Next Fox");
	set_ui_button_color(world, g_ins_next_btn, float4(0.16, 0.20, 0.21, 1.0));
	add_ui_child(world, ins_panel, g_ins_next_btn);

	g_ins_x_sl = create_ui_slider(world, 32.0, 410.0, 244.0, 26.0, -30.0, 30.0, 0.0);
	set_ui_slider_label(world, g_ins_x_sl, "Position X");
	add_ui_child(world, ins_panel, g_ins_x_sl);

	g_ins_y_sl = create_ui_slider(world, 32.0, 458.0, 244.0, 26.0, -1.0, 16.0, 0.0);
	set_ui_slider_label(world, g_ins_y_sl, "Position Y");
	add_ui_child(world, ins_panel, g_ins_y_sl);

	g_ins_z_sl = create_ui_slider(world, 32.0, 506.0, 244.0, 26.0, -30.0, 30.0, 0.0);
	set_ui_slider_label(world, g_ins_z_sl, "Position Z");
	add_ui_child(world, ins_panel, g_ins_z_sl);

	g_ins_yaw_sl = create_ui_slider(world, 32.0, 554.0, 244.0, 26.0, -180.0, 180.0, 0.0);
	set_ui_slider_label(world, g_ins_yaw_sl, "Yaw (degrees)");
	add_ui_child(world, ins_panel, g_ins_yaw_sl);

	g_ins_sc_sl = create_ui_slider(world, 32.0, 602.0, 244.0, 26.0, 0.01, 5.0, 0.05);
	set_ui_slider_label(world, g_ins_sc_sl, "Uniform scale");
	add_ui_child(world, ins_panel, g_ins_sc_sl);

	// ── NEW Native UI - Physics Toys & Spawner panel ─────────────────────────
	let toy_panel = create_ui_panel(world, 312.0, 296.0, 280.0, 480.0, "Physics Toys & Spawner");
	set_ui_panel_collapsible(world, toy_panel, true);

	let toy_intro = create_ui_label(world, 328.0, 336.0, 260.0, 20.0, "Drop primitives from above (tagged)");
	set_ui_font_size(world, toy_intro, 11.0);
	set_ui_text_color(world, toy_intro, float4(0.60, 0.66, 0.62, 1.0));
	add_ui_child(world, toy_panel, toy_intro);

	g_toy_sz_sl = create_ui_slider(world, 328.0, 378.0, 244.0, 26.0, 0.2, 2.0, 0.4);
	set_ui_slider_label(world, g_toy_sz_sl, "Toy size");
	add_ui_child(world, toy_panel, g_toy_sz_sl);

	g_toy_h_sl = create_ui_slider(world, 328.0, 426.0, 244.0, 26.0, 4.0, 24.0, 12.0);
	set_ui_slider_label(world, g_toy_h_sl, "Drop height");
	add_ui_child(world, toy_panel, g_toy_h_sl);

	g_btn_cube    = create_ui_button(world, 328.0, 462.0, 244.0, 32.0, "Spawn Cube @ Drop");
	g_btn_sphere  = create_ui_button(world, 328.0, 500.0, 244.0, 32.0, "Spawn Sphere @ Drop");
	g_btn_tower   = create_ui_button(world, 328.0, 538.0, 244.0, 32.0, "Stack Tower (5 boxes)");
	g_btn_rain    = create_ui_button(world, 328.0, 576.0, 244.0, 32.0, "Rain (4 spheres)");
	g_btn_bounce  = create_ui_button(world, 328.0, 614.0, 244.0, 32.0, "Bounce Test (cube, +Y vel)");
	set_ui_button_color(world, g_btn_cube,   float4(0.18, 0.22, 0.22, 1.0));
	set_ui_button_color(world, g_btn_sphere, float4(0.18, 0.22, 0.22, 1.0));
	set_ui_button_color(world, g_btn_tower,  float4(0.25, 0.29, 0.18, 1.0));
	set_ui_button_color(world, g_btn_rain,   float4(0.25, 0.29, 0.18, 1.0));
	set_ui_button_color(world, g_btn_bounce,  float4(0.30, 0.40, 0.19, 1.0));
	add_ui_child(world, toy_panel, g_btn_cube);
	add_ui_child(world, toy_panel, g_btn_sphere);
	add_ui_child(world, toy_panel, g_btn_tower);
	add_ui_child(world, toy_panel, g_btn_rain);
	add_ui_child(world, toy_panel, g_btn_bounce);

	g_btn_clear = create_ui_button(world, 328.0, 656.0, 244.0, 32.0, "Clear All Toys");
	set_ui_button_color(world, g_btn_clear,       float4(0.48, 0.18, 0.13, 1.0));
	set_ui_button_hover_color(world, g_btn_clear, float4(0.64, 0.25, 0.16, 1.0));
	set_ui_button_press_color(world, g_btn_clear, float4(0.34, 0.12, 0.09, 1.0));
	add_ui_child(world, toy_panel, g_btn_clear);

	// ── spawn a human (1 clip: idle) ───────────────────────────────────────
	g_player = entity_create(world);
	add_transform(world, g_player);
	set_transform(world, g_player, float3(0.0, 0.0, -6.0), float3(0.0, 180.0, 0.0), float3(0.05, 0.05, 0.05));
	load_model(world, g_player, "assets://models/Human/Human.mesh");
	g_player_idle_clip = get_current_animation(world, g_player);
	set_playback_speed(world, g_player, 1.0);
	add_tag(world, g_player, create_tag("player"));

	// Add walking animation (with root lock so the model animates in place),
	// then compile all clips into the GPU database.
	g_player_walk_clip = add_animation(world, g_player, "assets://animations/Walking_mixamo.com.anim", true);
	if (g_player_walk_clip >= 0)
	{
		compile_animations(world, g_player);
	}

	// Init default bindings, then register the new sandbox actions.
	init_default_bindings();
	register_action("inspector_prev",  KEY_LEFT,   0);
	register_action("inspector_next",  KEY_RIGHT,  0);
	register_action("spawn_cube_key",  KEY_1,      0);
	register_action("spawn_sphere_key", KEY_2,    0);
	register_action("clear_toys_key",  KEY_X,      0);
	register_action("spawn_tower_key", KEY_T,      0);

	// Pick first fox into the inspector after attach.
	g_ins_idx = 0;
	refresh_inspector(world);
}

// ── Helper: format time of day as HH:MM ───────────────────────────────────────

def format_time(hours : float) : string
{
	let h = int(hours) % 24;
	let m = int((hours - float(int(hours))) * 60.0) % 60;
	return "{h:02}:{m:02}";
}

// ── on_update ─────────────────────────────────────────────────────────────────

[export]
def on_update(world : World?)
{
	let dt = get_delta_time();

	// ── Animate moving point light (index 2 in point light list) ───────────────
	g_time += dt;
	let orbit_x = cos(g_time * 0.3) * g_orbit_radius;
	let orbit_z = sin(g_time * 0.3) * g_orbit_radius;
	let orbit_y = g_orbit_height;
	set_point_light_position(2, float3(orbit_x, orbit_y, orbit_z));

	// ── Plasma sphere animation ────────────────────────────────────────────
	if (entity_valid(g_plasma_sphere))
	{
		let bob = sin(g_time * 0.8) * 0.8;
		var pos = get_position(world, g_plasma_sphere);
		pos.y = 6.0 + bob;
		set_position(world, g_plasma_sphere, pos);

		let hue = sin(g_time * 0.3) * 0.5 + 0.5;
		set_effect_color(world, g_plasma_sphere, 0.5 + hue * 0.5, 0.2, 0.8 - hue * 0.4);
		set_effect_speed(world, g_plasma_sphere, 0.6 + sin(g_time * 0.2) * 0.4);
		set_effect_intensity(world, g_plasma_sphere, 0.7 + sin(g_time * 0.5) * 0.3);
	}

	// ── Read UI controls ──────────────────────────────────────────────────────
	g_speed_scale  = get_ui_slider_value(world, g_speed_slider);
	g_rotate_foxes = get_ui_checked(world, g_rotate_cb);

	// Spawn a new fox when scene-controls button clicked
	if (get_ui_clicked(world, g_spawn_btn))
	{
		let angle  = float(g_fox_count) * 0.618 * 6.2832;
		let radius = 3.0 + sqrt(float(g_fox_count)) * 1.8;
		let pos    = float3(cos(angle) * radius, 0.0, sin(angle) * radius);
		let yaw    = angle * 57.2958 + 90.0;

		let fox = entity_create(world);
		add_transform(world, fox);
		set_transform(world, fox, pos, float3(0.0, yaw, 0.0), float3(0.05, 0.05, 0.05));
		load_model(world, fox, "assets://models/Fox/Fox.mesh");
		set_animation(world, fox, 2);
		set_playback_speed(world, fox, 0.7);
		set_anim_time(world, fox, float(g_fox_count) * 0.3);
		add_tag(world, fox, g_fox_tag);
		g_fox_count++;

		set_ui_button_label(world, g_spawn_btn, "Spawn Fox ({g_fox_count})");
	}

	// Rotate all foxes if enabled (uses for_each_with_tag_transform so the block
	// gets the entity's pos/euler/scale without needing extra getter calls).
	if (g_rotate_foxes)
	{
		for_each_with_tag_transform(world, g_fox_tag) $(e : uint; pos : float3; euler : float3; scale : float3)
		{
			let r = sqrt(pos.x * pos.x + pos.z * pos.z);
			if (r > 0.5)
			{
				let na  = atan2(pos.x, pos.z) + dt * g_speed_scale * 0.6;
				let np  = float3(sin(na) * r, pos.y, cos(na) * r);
				let ny  = na * 57.2958 + 90.0;
				set_transform(world, e, np, float3(euler.x, ny, euler.z), scale);
			}
		};
	}

	// Switch fox animations
	let anim_clip = g_rotate_foxes ? 2 : 0;
	for_each_with_tag(world, g_fox_tag) $(e : uint)
	{
		if (get_current_animation(world, e) != anim_clip)
		{
			set_animation(world, e, anim_clip);
			set_playback_speed(world, e, g_rotate_foxes ? 0.7 : 1.0);
		}
	};

	// ── Entity inspector UI logic ─────────────────────────────────────────────
	if (get_ui_clicked(world, g_ins_prev_btn))
	{
		if (g_ins_count == 0) { g_ins_idx = -1; }
		else { g_ins_idx = (g_ins_idx - 1 + g_ins_count) % g_ins_count; }
		refresh_inspector(world);
	}
	if (get_ui_clicked(world, g_ins_next_btn))
	{
		if (g_ins_count == 0) { g_ins_idx = -1; }
		else { g_ins_idx = (g_ins_idx + 1) % g_ins_count; }
		refresh_inspector(world);
	}
	if (is_action_pressed("inspector_prev"))
	{
		if (g_ins_count == 0) { g_ins_idx = -1; }
		else { g_ins_idx = (g_ins_idx - 1 + g_ins_count) % g_ins_count; }
		refresh_inspector(world);
	}
	if (is_action_pressed("inspector_next"))
	{
		if (g_ins_count == 0) { g_ins_idx = -1; }
		else { g_ins_idx = (g_ins_idx + 1) % g_ins_count; }
		refresh_inspector(world);
	}

	// Apply slider edits back to the selected entity (write-on-change).
	if (g_ins_entity != 0u && entity_valid(g_ins_entity))
	{
		let sx  = get_ui_slider_value(world, g_ins_x_sl);
		let sy  = get_ui_slider_value(world, g_ins_y_sl);
		let sz  = get_ui_slider_value(world, g_ins_z_sl);
		let syw = get_ui_slider_value(world, g_ins_yaw_sl);
		let ssc = get_ui_slider_value(world, g_ins_sc_sl);

		var dirty = false;
		if (abs(sx  - g_ins_x)   > 0.001) { g_ins_x   = sx;  dirty = true; }
		if (abs(sy  - g_ins_y)   > 0.001) { g_ins_y   = sy;  dirty = true; }
		if (abs(sz  - g_ins_z)   > 0.001) { g_ins_z   = sz;  dirty = true; }
		if (abs(syw - g_ins_yaw) > 0.001) { g_ins_yaw = syw; dirty = true; }
		if (abs(ssc - g_ins_sc)  > 0.001) { g_ins_sc  = ssc; dirty = true; }

		if (dirty)
		{
			set_transform(world, g_ins_entity, float3(g_ins_x, g_ins_y, g_ins_z),
				float3(0.0, g_ins_yaw, 0.0), float3(g_ins_sc, g_ins_sc, g_ins_sc));
		}
	}
	elif (g_ins_entity != 0u)
	{
		// Selected entity went away (cleared/destroyed). Reset selection.
		g_ins_entity = 0u;
		set_ui_text(world, g_ins_label_idx, "Selected: <none>");
	}

	// ── Player controller & third-person camera ───────────────────────────────
	var inputX = 0.0; var inputZ = 0.0;
	if (is_action_down("move_forward"))  { inputZ += 1.0; }
	if (is_action_down("move_backward")) { inputZ -= 1.0; }
	if (is_action_down("move_left"))     { inputX -= 1.0; }
	if (is_action_down("move_right"))    { inputX += 1.0; }
	update_player_controller(world, dt, g_player, g_player_idle_clip, g_player_walk_clip, inputX, inputZ, g_cam);
	update_third_person_camera(world, g_cam, g_player);

	// ── Day/Night cycle controls ──────────────────────────────────────────────
	g_manual_mode = get_ui_checked(world, g_manual_cb);
	set_day_night_enabled(!g_manual_mode);

	if (g_manual_mode)
	{
		let new_time = get_ui_slider_value(world, g_time_slider);
		if (abs(new_time - g_time_of_day) > 0.01)
		{
			g_time_of_day = new_time;
			set_time_of_day(g_time_of_day);
		}
	}
	else
	{
		g_time_speed = get_ui_slider_value(world, g_speed_slider_dn);
		set_time_speed(g_time_speed);
		g_time_of_day = get_time_of_day();
		set_ui_slider_value(world, g_time_slider, g_time_of_day);
	}

	let time_str = format_time(g_time_of_day);
	set_ui_text(world, g_time_label, "Time: {time_str}");

	let sun_dir = get_sun_direction();
	let sun_str = "Sun: ({sun_dir.x:.2f}, {sun_dir.y:.2f}, {sun_dir.z:.2f})";
	set_ui_text(world, g_sun_dir_label, sun_str);

	// ── Physics toys UI ──────────────────────────────────────────────────────
	let toy_sz = get_ui_slider_value(world, g_toy_sz_sl);
	let toy_h  = get_ui_slider_value(world, g_toy_h_sl);

	if (get_ui_clicked(world, g_btn_cube) || is_action_pressed("spawn_cube_key"))
	{
		spawn_toy(world, false, float3(0.0, toy_h, 0.0), toy_sz);
	}
	if (get_ui_clicked(world, g_btn_sphere) || is_action_pressed("spawn_sphere_key"))
	{
		spawn_toy(world, true, float3(0.0, toy_h, 0.0), toy_sz);
	}
	if (get_ui_clicked(world, g_btn_tower) || is_action_pressed("spawn_tower_key"))
	{
		spawn_tower(world);
	}
	if (get_ui_clicked(world, g_btn_rain))
	{
		spawn_rain(world);
	}
	if (get_ui_clicked(world, g_btn_bounce))
	{
		// Spawn a cube with an upward velocity for a bounce test.
		let e = entity_create(world);
		add_transform(world, e);
		set_transform(world, e, float3(0.0, 0.5, 0.0), float3(0.0, 0.0, 0.0), float3(toy_sz, toy_sz, toy_sz));
		add_mesh(world, e, g_cube_mesh);
		add_box_body(world, e, float3(toy_sz * 0.5, toy_sz * 0.5, toy_sz * 0.5), true);
		add_tag(world, e, g_phys_toy_tag);
		set_linear_velocity(world, e, 0.0, 12.0, 0.0);
	}
	if (get_ui_clicked(world, g_btn_clear) || is_action_pressed("clear_toys_key"))
	{
		clear_toys(world);
	}

	// ── Keyboard shortcuts ────────────────────────────────────────────────────
	if (is_action_pressed("toggle_rotate"))
	{
		g_rotate_foxes = !g_rotate_foxes;
		set_ui_checked(world, g_rotate_cb, g_rotate_foxes);
	}

	if (is_action_down("speed_up"))
	{
		g_speed_scale = clamp(g_speed_scale + dt * 2.0, 0.1, 5.0);
		set_ui_slider_value(world, g_speed_slider, g_speed_scale);
	}
	if (is_action_down("speed_down"))
	{
		g_speed_scale = clamp(g_speed_scale - dt * 2.0, 0.1, 5.0);
		set_ui_slider_value(world, g_speed_slider, g_speed_scale);
	}

	if (is_action_pressed("toggle_manual"))
	{
		g_manual_mode = !g_manual_mode;
		set_ui_checked(world, g_manual_cb, g_manual_mode);
		set_day_night_enabled(!g_manual_mode);
	}

	if (is_action_down("time_up") && g_manual_mode)
	{
		g_time_of_day = clamp(g_time_of_day + dt * 2.0, 0.0, 24.0);
		if (g_time_of_day >= 24.0)
		{
			g_time_of_day = 0.0;
		}
		set_time_of_day(g_time_of_day);
		set_ui_slider_value(world, g_time_slider, g_time_of_day);
	}
	if (is_action_down("time_down") && g_manual_mode)
	{
		g_time_of_day = g_time_of_day - dt * 2.0;
		if (g_time_of_day < 0.0)
		{
			g_time_of_day = 24.0;
		}
		set_time_of_day(g_time_of_day);
		set_ui_slider_value(world, g_time_slider, g_time_of_day);
	}

	if (is_action_pressed("time_noon"))
	{
		g_time_of_day = 12.0;
		set_time_of_day(12.0);
		set_ui_slider_value(world, g_time_slider, 12.0);
	}

	if (is_action_pressed("time_midnight"))
	{
		g_time_of_day = 0.0;
		set_time_of_day(0.0);
		set_ui_slider_value(world, g_time_slider, 0.0);
	}
}

// ── on_detach ─────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
}
