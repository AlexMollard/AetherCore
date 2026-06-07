options gen2;

require world;
require renderer;
require camera;
require physics;
require effects;
require input;
require input_bindings;
require ui;
require math;
require animation;
require systems/fox_system;

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
var g_player_ik_enabled : bool = true;

// ── Tags ─────────────────────────────────────────────────────────────────────

var g_fox_tag : uint = 0u;
var g_ground_tag : uint = 0u;
var g_center_tag : uint = 0u;

// ── on_attach ─────────────────────────────────────────────────────────────────

[export]
def on_attach(world : World?)
{
	g_cube_mesh   = create_mesh(world, "cube");
	g_sphere_mesh = create_mesh(world, "sphere");
	g_time        = 0.0;

	// ── Create tags ─────────────────────────────────────────────────────────
	g_fox_tag    = create_tag("fox");
	g_ground_tag = create_tag("ground");
	g_center_tag = create_tag("center");

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
	let cam = create_orbit_camera(float3(0.0, 10.0, 28.0), float3(0.0, 1.0, 0.0), 50.0);
	set_main_camera(cam);

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

	// ─ RIGHT ZONE: sphere grid under cool point light ──────────────────────
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

	// ─ CENTER ZONE: tall pillar under spot light ──────────────────────────
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

	// ─ Physics box (drops from above, right zone) ──────────────────────────
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

	let sep_lbl = create_ui_label(world, 16.0, 88.0, 260.0, 20.0, "Fox rotation speed");
	set_ui_font_size(world, sep_lbl, 12.0);
	set_ui_text_color(world, sep_lbl, float4(0.65, 0.65, 0.72, 1.0));
	add_ui_child(world, panel, sep_lbl);

	g_speed_slider = create_ui_slider(world, 16.0, 112.0, 260.0, 28.0, 0.1, 5.0, 1.0);
	add_ui_child(world, panel, g_speed_slider);

	g_rotate_cb = create_ui_checkbox(world, 16.0, 152.0, 260.0, 28.0, "Rotate foxes", true);
	add_ui_child(world, panel, g_rotate_cb);

	g_spawn_btn = create_ui_button(world, 16.0, 192.0, 260.0, 40.0, "Spawn Fox");
	set_ui_button_color(world, g_spawn_btn,       float4(0.15, 0.38, 0.20, 1.0));
	set_ui_button_hover_color(world, g_spawn_btn, float4(0.20, 0.50, 0.27, 1.0));
	set_ui_button_press_color(world, g_spawn_btn, float4(0.10, 0.28, 0.15, 1.0));
	add_ui_child(world, panel, g_spawn_btn);

	// ── Native UI - Day/Night Controls panel ────────────────────────────────
	let dn_panel = create_ui_panel(world, 312.0, 16.0, 280.0, 260.0, "Day / Night");
	set_ui_panel_collapsible(world, dn_panel, true);

	g_time_label = create_ui_label(world, 328.0, 56.0, 260.0, 24.0, "Time: 12:00");
	set_ui_font_size(world, g_time_label, 14.0);
	set_ui_text_color(world, g_time_label, float4(1.0, 0.95, 0.70, 1.0));
	add_ui_child(world, dn_panel, g_time_label);

	g_sun_dir_label = create_ui_label(world, 328.0, 84.0, 260.0, 20.0, "Sun: (0.00, 1.00, 0.00)");
	set_ui_font_size(world, g_sun_dir_label, 11.0);
	set_ui_text_color(world, g_sun_dir_label, float4(0.65, 0.65, 0.72, 1.0));
	add_ui_child(world, dn_panel, g_sun_dir_label);

	let tod_lbl = create_ui_label(world, 328.0, 112.0, 260.0, 20.0, "Time of day (hours)");
	set_ui_font_size(world, tod_lbl, 12.0);
	set_ui_text_color(world, tod_lbl, float4(0.65, 0.65, 0.72, 1.0));
	add_ui_child(world, dn_panel, tod_lbl);

	g_time_slider = create_ui_slider(world, 328.0, 136.0, 260.0, 28.0, 0.0, 24.0, 12.0);
	add_ui_child(world, dn_panel, g_time_slider);

	let spd_lbl = create_ui_label(world, 328.0, 172.0, 260.0, 20.0, "Time speed (sec/sec)");
	set_ui_font_size(world, spd_lbl, 12.0);
	set_ui_text_color(world, spd_lbl, float4(0.65, 0.65, 0.72, 1.0));
	add_ui_child(world, dn_panel, spd_lbl);

	g_speed_slider_dn = create_ui_slider(world, 328.0, 196.0, 260.0, 28.0, 0.0, 300.0, 60.0);
	add_ui_child(world, dn_panel, g_speed_slider_dn);

	g_manual_cb = create_ui_checkbox(world, 328.0, 232.0, 260.0, 28.0, "Manual mode (pause auto)", false);
	add_ui_child(world, dn_panel, g_manual_cb);

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
		print("Walking animation added as clip {g_player_walk_clip}\n");
		compile_animations(world, g_player);
		// IK bone indices and leg lengths are initialized from C++ after compile.
		set_ik_enabled(world, g_player, true);
		print("IK foot planting enabled for player\n");
	}
	else
	{
		print("Failed to add walking animation\n");
	}

	init_default_bindings();
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

	// Spawn a new fox when button clicked
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

	// Rotate all foxes if enabled
	if (g_rotate_foxes)
	{
		for_each_with_tag(world, g_fox_tag) $(e : uint)
		{
			let pos    = get_position(world, e);
			let euler  = get_euler(world, e);
			let scale  = get_scale(world, e);
			let angle  = atan2(pos.x, pos.z);
			let r      = sqrt(pos.x * pos.x + pos.z * pos.z);
			if (r > 0.5)
			{
				let na  = angle + dt * g_speed_scale * 0.6;
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

	// reset the player pos
	set_transform(world, g_player, float3(0.0, 0.0, -6.0), float3(0.0, 180.0, 0.0), float3(0.05, 0.05, 0.05));

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

	// ── Toggle player walking animation with 'W' key ────────────────────────
	let currentClip = get_current_animation(world, g_player);

	if (is_action_down("move_forward"))
	{
		if (g_player_walk_clip >= 0)
		{
			// Use blend transition for smooth cross-fade to walk
			set_animation_blend(world, g_player, g_player_walk_clip, 3.0);
			set_playback_speed(world, g_player, 0.8);
		}
	}
	else
	{
		// Use blend transition back to idle
		if (g_player_idle_clip >= 0)
		{
			set_animation_blend(world, g_player, g_player_idle_clip, 3.0);
			set_playback_speed(world, g_player, 1.0);
		}
	}

	// ── Toggle IK foot planting with 'I' key ─────────────────────────────
	if (is_action_pressed("toggle_ik"))
	{
		g_player_ik_enabled = !g_player_ik_enabled;
		set_ik_enabled(world, g_player, g_player_ik_enabled);
		var ikState = g_player_ik_enabled ? "ENABLED" : "DISABLED";
		var footContact = get_foot_contact(world, g_player, 0) ? "L foot planted" : "L foot in air";
		print("IK {ikState} ({footContact})\n");
	}
}

// ── on_detach ─────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
}
