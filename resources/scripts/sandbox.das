options gen2;

require world;
require renderer;
require camera;
require physics;
require effects;
require input;
require keycodes;
require input_bindings;
require math;
require animation;
require systems/fox_system;
require systems/character_controller;
require systems/third_person_camera;

// ── Scene state ───────────────────────────────────────────────────────────────

var g_fox_count    : int   = 0;
var g_speed_scale  : float = 1.0;
var g_rotate_foxes : bool  = true;
var g_cube_mesh   : uint  = 0u;
var g_sphere_mesh : uint  = 0u;

// ── Moving light state ─────────────────────────────────────────────────────────

var g_time           : float = 0.0;
var g_orbit_radius   : float = 20.0;
var g_orbit_height   : float = 3.0;

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

// Shared enumeration scratch (avoids needing array mutation inside blocks).
var g_enum_count  : int  = 0;
var g_enum_target : int  = -1;
var g_enum_result : uint = 0u;

// ── Physics toy / spawner state ────────────────────────────────────────────────

var g_toy_size        : float = 0.4;
var g_toy_drop_height : float = 12.0;

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
	let sz = g_toy_size;
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
	let sz = g_toy_size;
	let h  = g_toy_drop_height;
	for (i in 0..4)
	{
		let ang = float(i) * 1.2566; // 72 deg spacing
		let x   = cos(ang) * 4.0 + float(i) * 0.7;
		let z   = sin(ang) * 4.0;
		spawn_toy(world, true, float3(x, h, z), sz);
	}
}

def spawn_bounce_test(world : World?)
{
	let sz = g_toy_size;
	let e = entity_create(world);
	add_transform(world, e);
	set_transform(world, e, float3(0.0, 0.5, 0.0), float3(0.0, 0.0, 0.0), float3(sz, sz, sz));
	add_mesh(world, e, g_cube_mesh);
	add_box_body(world, e, float3(sz * 0.5, sz * 0.5, sz * 0.5), true);
	add_tag(world, e, g_phys_toy_tag);
	set_linear_velocity(world, e, 0.0, 12.0, 0.0);
}

def spawn_fox(world : World?)
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
	register_action("spawn_cube_key",  KEY_1,      0);
	register_action("spawn_sphere_key", KEY_2,    0);
	register_action("clear_toys_key",  KEY_X,      0);
	register_action("spawn_tower_key", KEY_T,      0);
	register_action("spawn_fox_key",   KEY_F,      0);
	register_action("spawn_rain_key",  KEY_3,      0);
	register_action("bounce_test_key", KEY_B,      0);
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

	if (is_action_pressed("spawn_fox_key"))
	{
		spawn_fox(world);
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

	// ── Player controller & third-person camera ───────────────────────────────
	var inputX = 0.0; var inputZ = 0.0;
	if (is_action_down("move_forward"))  { inputZ += 1.0; }
	if (is_action_down("move_backward")) { inputZ -= 1.0; }
	if (is_action_down("move_left"))     { inputX -= 1.0; }
	if (is_action_down("move_right"))    { inputX += 1.0; }
	update_player_controller(world, dt, g_player, g_player_idle_clip, g_player_walk_clip, inputX, inputZ, g_cam);
	update_third_person_camera(world, g_cam, g_player);

	// ── Physics toys keyboard shortcuts ──────────────────────────────────────
	let toy_sz = g_toy_size;
	let toy_h  = g_toy_drop_height;

	if (is_action_pressed("spawn_cube_key"))
	{
		spawn_toy(world, false, float3(0.0, toy_h, 0.0), toy_sz);
	}
	if (is_action_pressed("spawn_sphere_key"))
	{
		spawn_toy(world, true, float3(0.0, toy_h, 0.0), toy_sz);
	}
	if (is_action_pressed("spawn_tower_key"))
	{
		spawn_tower(world);
	}
	if (is_action_pressed("spawn_rain_key"))
	{
		spawn_rain(world);
	}
	if (is_action_pressed("bounce_test_key"))
	{
		spawn_bounce_test(world);
	}
	if (is_action_pressed("clear_toys_key"))
	{
		clear_toys(world);
	}

	// ── Keyboard shortcuts ────────────────────────────────────────────────────
	if (is_action_pressed("toggle_rotate"))
	{
		g_rotate_foxes = !g_rotate_foxes;
	}

	if (is_action_down("speed_up"))
	{
		g_speed_scale = clamp(g_speed_scale + dt * 2.0, 0.1, 5.0);
	}
	if (is_action_down("speed_down"))
	{
		g_speed_scale = clamp(g_speed_scale - dt * 2.0, 0.1, 5.0);
	}

}

// ── on_detach ─────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
}
