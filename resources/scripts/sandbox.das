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

var g_plasma_core : uint = 0u;
var g_player : uint = 0u;
var g_player_idle_clip : int = 0;
var g_player_walk_clip : int = 1;
var g_cam : uint = 0u;

// ── Tags ─────────────────────────────────────────────────────────────────────

var g_fox_tag : uint = 0u;
var g_ground_tag : uint = 0u;
var g_center_tag : uint = 0u;
var g_phys_toy_tag : uint = 0u;

// Shared enumeration scratch
var g_enum_count  : int  = 0;
var g_enum_target : int  = -1;
var g_enum_result : uint = 0u;

// ── Physics toy / spawner state ────────────────────────────────────────────────

var g_toy_size        : float = 0.6;
var g_toy_drop_height : float = 25.0;

// ── Helpers ────────────────────────────────────────────────────────────────────

// Generic static architecture builder
def build_block(world : World?; pos : float3; rot : float3; size : float3) : uint
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, pos, rot, size);
    add_mesh(world, e, g_cube_mesh);
    add_box_body(world, e, size * 0.5, false);
    add_tag(world, e, g_ground_tag);
    return e;
}

// Count entities carrying the given tag
def count_tagged(world : World?; tag : uint) : int
{
    g_enum_count = 0;
    for_each_with_tag(world, tag) $(e : uint)
    {
        g_enum_count += 1;
    };
    return g_enum_count;
}

// Find the Nth (0-based) entity with the given tag
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

// Spawn a single dynamic primitive
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

// Destroy every entity tagged as a physics toy
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

// Stack a vertical tower of dynamic boxes
def spawn_tower(world : World?)
{
    let sz = g_toy_size;
    for (i in 0..8)
    {
        let y = 1.0 + float(i) * sz * 1.05;
        let jx = (float(i % 2) - 0.5) * 0.04;
        spawn_toy(world, false, float3(jx, y, 0.0), sz);
    }
}

// Scatter 8 dynamic spheres above the scene
def spawn_rain(world : World?)
{
    let sz = g_toy_size;
    let h  = g_toy_drop_height;
    for (i in 0..8)
    {
        let ang = float(i) * 0.7854; // 45 deg spacing
        let x   = cos(ang) * 8.0 + float(i) * 0.5;
        let z   = sin(ang) * 8.0;
        spawn_toy(world, true, float3(x, h, z), sz);
    }
}

def spawn_bounce_test(world : World?)
{
    let sz = g_toy_size;
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 0.0), float3(sz, sz, sz));
    add_mesh(world, e, g_cube_mesh);
    add_box_body(world, e, float3(sz * 0.5, sz * 0.5, sz * 0.5), true);
    add_tag(world, e, g_phys_toy_tag);
    set_linear_velocity(world, e, 0.0, 20.0, 0.0);
}

def spawn_fox_at(world : World?; pos : float3; yaw : float)
{
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

def spawn_fox(world : World?)
{
    // Spawn new foxes in a chaotic spiral around the center
    let angle  = float(g_fox_count) * 0.618 * 6.2832;
    let radius = 8.0 + sqrt(float(g_fox_count)) * 1.8;
    let pos    = float3(cos(angle) * radius, 1.0, sin(angle) * radius);
    let yaw    = angle * 57.2958 + 90.0;
    spawn_fox_at(world, pos, yaw);
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

    // ── Lighting: Dramatic Twilight Atmosphere ──────────────────────────────
    clear_lights();
    set_ambient(float3(0.12, 0.10, 0.18)); // Dark purple twilight ambient

    // Setting sun casting long shadows
    set_sun(float3(0.6, -0.4, 0.7), 2.0, float3(1.0, 0.6, 0.3));
    set_sky(float3(0.8, 0.3, 0.1), float3(0.1, 0.05, 0.2)); // Red/Purple sunset sky

    // ── Base Ground & World Bounds ───────────────────────────────────────────
    // Massive void floor
    build_block(world, float3(0.0, -5.0, 0.0), float3(0.0, 0.0, 0.0), float3(500.0, 10.0, 500.0));
    
    // ── CENTRAL NEXUS PLAZA ──────────────────────────────────────────────────
    // Main raised platform
    build_block(world, float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 0.0), float3(60.0, 2.0, 60.0));
    
    // Central Altar (Stepped Pyramid)
    for (i in 0..4)
    {
        let s = 12.0 - float(i) * 2.0;
        let y = 0.0 + float(i) * 2.0;
        build_block(world, float3(0.0, y, 0.0), float3(0.0, 0.0, 0.0), float3(s, 2.0, s));
    }
    
    // Orbital Ring on Altar
    let ring_r = 6.0;
    for (i in 0..8)
    {
        let ang = float(i) * 0.7854;
        let x = cos(ang) * ring_r;
        let z = sin(ang) * ring_r;
        build_block(world, float3(x, 9.0, z), float3(0.0, ang * 57.2958, 0.0), float3(0.5, 4.0, 1.5));
    }

    // ── EASTERN SKY TEMPLE (X = 80) ──────────────────────────────────────────
    // Grand Staircase
    for (i in 0..10)
    {
        let h = float(i + 1) * 2.0;
        let x = 35.0 + float(i) * 4.0;
        build_block(world, float3(x, h - 1.0, 0.0), float3(0.0, 0.0, 0.0), float3(4.0, h, 20.0));
    }
    
    // Temple Platform
    build_block(world, float3(85.0, 20.0, 0.0), float3(0.0, 0.0, 0.0), float3(40.0, 2.0, 60.0));
    
    // Temple Roof & Pillars
    build_block(world, float3(85.0, 35.0, 0.0), float3(0.0, 0.0, 0.0), float3(40.0, 2.0, 60.0));
    for (i in 0..6)
    {
        let z = -25.0 + float(i) * 10.0;
        build_block(world, float3(70.0, 27.0, z), float3(0.0, 0.0, 0.0), float3(2.0, 14.0, 2.0));
        build_block(world, float3(100.0, 27.0, z), float3(0.0, 0.0, 0.0), float3(2.0, 14.0, 2.0));
    }
    
    // Heavenly spotlights shining down through the temple roof
    add_spot_light(float3(85.0, 30.0, -20.0), float3(0.8, 0.9, 1.0), 25.0, 50.0, float3(0.0, -1.0, 0.0), 0.4, 0.8, true);
    add_spot_light(float3(85.0, 30.0, 20.0), float3(0.8, 0.9, 1.0), 25.0, 50.0, float3(0.0, -1.0, 0.0), 0.4, 0.8, true);

    // ── SOUTHERN CATACOMBS PIT (Z = -60) ─────────────────────────────────────
    // Sunken floor
    build_block(world, float3(0.0, -3.0, -60.0), float3(0.0, 0.0, 0.0), float3(50.0, 2.0, 40.0));
    // Retaining walls
    build_block(world, float3(0.0, 0.0, -80.0), float3(0.0, 0.0, 0.0), float3(50.0, 8.0, 2.0));
    build_block(world, float3(-24.0, 0.0, -60.0), float3(0.0, 0.0, 0.0), float3(2.0, 8.0, 40.0));
    build_block(world, float3(24.0, 0.0, -60.0), float3(0.0, 0.0, 0.0), float3(2.0, 8.0, 40.0));
    
    // Eerie green point lights in the pit
    add_point_light(float3(-15.0, 2.0, -70.0), float3(0.2, 1.0, 0.4), 15.0, 30.0, true);
    add_point_light(float3(15.0, 2.0, -50.0), float3(0.2, 1.0, 0.4), 15.0, 30.0, true);
    add_point_light(float3(0.0, 2.0, -60.0), float3(0.8, 0.2, 0.8), 20.0, 40.0, true);

    // ── WESTERN PILLAR MAZE (X = -60) ────────────────────────────────────────
    build_block(world, float3(-60.0, -1.0, 0.0), float3(0.0, 0.0, 0.0), float3(60.0, 2.0, 80.0));
    
    // Procedural-ish maze of pillars
    for (row in 0..5)
    {
        for (col in 0..6)
        {
            if ((row + col) % 2 == 0 || row == 2)
            {
                let x = -85.0 + float(col) * 8.0;
                let z = -32.0 + float(row) * 12.0;
                build_block(world, float3(x, 5.0, z), float3(0.0, 0.0, 0.0), float3(2.0, 10.0, 2.0));
            }
        }
    }
    
    // Lanterns in the maze
    add_point_light(float3(-70.0, 5.0, 0.0), float3(1.0, 0.8, 0.3), 10.0, 25.0, true);
    add_point_light(float3(-90.0, 5.0, 20.0), float3(1.0, 0.8, 0.3), 10.0, 25.0, true);

    // ── DYNAMIC ELEMENTS ─────────────────────────────────────────────────────
    
    // Massive Plasma Core hovering over the Nexus Altar
    g_plasma_core = entity_create(world);
    add_transform(world, g_plasma_core);
    set_transform(world, g_plasma_core, float3(0.0, 20.0, 0.0), float3(0.0, 0.0, 0.0), float3(4.0, 4.0, 4.0));
    add_mesh(world, g_plasma_core, g_sphere_mesh);
    set_entity_effect(world, g_plasma_core, "plasma");
    set_effect_color(world, g_plasma_core, 1.0, 0.4, 0.9);
    set_effect_speed(world, g_plasma_core, 1.2);
    set_effect_intensity(world, g_plasma_core, 2.0);
    add_tag(world, g_plasma_core, g_center_tag);
    
    // Orbiting point light for the core
    add_point_light(float3(0.0, 20.0, 0.0), float3(1.0, 0.2, 0.8), 25.0, 60.0, true); // Index 5

    // Initial Physics Toys: A massive Jenga tower in the plaza
    for (i in 0..10)
    {
        let y = 0.5 + float(i) * 1.2;
        let rot_y = (i % 2 == 0) ? 0.0 : 90.0;
        let sz = 1.0;
        let e = entity_create(world);
        add_transform(world, e);
        set_transform(world, e, float3(15.0, y, 15.0), float3(0.0, rot_y, 0.0), float3(sz * 2.0, sz * 0.5, sz));
        add_mesh(world, e, g_cube_mesh);
        add_box_body(world, e, float3(sz, sz * 0.25, sz * 0.5), true);
        add_tag(world, e, g_phys_toy_tag);
    }

    // Initial Physics Toys: Boulders in the catacombs
    for (i in 0..5)
    {
        spawn_toy(world, true, float3(-10.0 + float(i) * 5.0, 5.0, -60.0), 1.5);
    }

    // ── Foxes (Patrolling the Nexus) ─────────────────────────────────────────
    let fox_ring_count = 8;
    for (i in 0..fox_ring_count)
    {
        let ang = float(i) * (6.2832 / float(fox_ring_count));
        let x = cos(ang) * 12.0;
        let z = sin(ang) * 12.0;
        spawn_fox_at(world, float3(x, 1.0, z), ang * 57.2958 + 90.0);
    }
    g_fox_count = fox_ring_count;

    // ── spawn a human player (At the start of the Eastern Stairs) ─────────────
    g_player = entity_create(world);
    add_transform(world, g_player);
    set_transform(world, g_player, float3(25.0, 1.0, 0.0), float3(0.0, -90.0, 0.0), float3(0.05, 0.05, 0.05));
    load_model(world, g_player, "assets://models/Human/Human.mesh");
    g_player_idle_clip = get_current_animation(world, g_player);
    set_playback_speed(world, g_player, 1.0);
    add_tag(world, g_player, create_tag("player"));

    g_player_walk_clip = add_animation(world, g_player, "assets://animations/Walking_mixamo.com.anim", true);
    if (g_player_walk_clip >= 0)
    {
        compile_animations(world, g_player);
    }

    // ── Camera & Input ───────────────────────────────────────────────────────
    g_cam = create_orbit_camera(float3(0.0, 40.0, 60.0), float3(0.0, 10.0, 0.0), 80.0);
    set_main_camera(g_cam);

    init_default_bindings();
    register_action("spawn_cube_key",  KEY_1,      0);
    register_action("spawn_sphere_key", KEY_2,    0);
    register_action("clear_toys_key",  KEY_X,      0);
    register_action("spawn_tower_key", KEY_T,      0);
    register_action("spawn_fox_key",   KEY_F,      0);
    register_action("spawn_rain_key",  KEY_3,      0);
    register_action("bounce_test_key", KEY_B,      0);
	register_action("sprint_key", KEY_LEFT_SHIFT, 0);
}

// ── on_update ─────────────────────────────────────────────────────────────────

[export]
def on_update(world : World?)
{
    let dt = get_delta_time();
    g_time += dt;

    // ── Plasma Core & Light Orbit ────────────────────────────────────────────
    if (entity_valid(g_plasma_core))
    {
        // Bobbing motion
        var pos = get_position(world, g_plasma_core);
        pos.y = 20.0 + sin(g_time * 0.8) * 3.0;
        pos.x = sin(g_time * 0.3) * 2.0;
        pos.z = cos(g_time * 0.3) * 2.0;
        set_position(world, g_plasma_core, pos);

        // Color shifting
        let hue = sin(g_time * 0.5) * 0.5 + 0.5;
        set_effect_color(world, g_plasma_core, 0.5 + hue * 0.5, 0.2, 0.8 - hue * 0.4);
        set_effect_speed(world, g_plasma_core, 0.6 + sin(g_time * 0.2) * 0.4);
        set_effect_intensity(world, g_plasma_core, 1.5 + sin(g_time * 0.5) * 0.5);
        
        // Orbit the light around the core
        let orbit_x = pos.x + cos(g_time * 2.0) * 5.0;
        let orbit_z = pos.z + sin(g_time * 2.0) * 5.0;
        let orbit_y = pos.y + sin(g_time * 3.0) * 2.0;
        set_point_light_position(5, float3(orbit_x, orbit_y, orbit_z));
    }

    if (is_action_pressed("spawn_fox_key"))
    {
        spawn_fox(world);
    }

    // ── Fox Rotation Logic ───────────────────────────────────────────────────
    if (g_rotate_foxes)
    {
        for_each_with_tag_transform(world, g_fox_tag) $(e : uint; pos : float3; euler : float3; scale : float3)
        {
            let r = sqrt(pos.x * pos.x + pos.z * pos.z);
            if (r > 0.5)
            {
                // Expand the ring slightly over time to create a vortex effect
                let target_r = 12.0 + sin(g_time * 0.2) * 4.0;
                let current_r = r + (target_r - r) * dt * 0.5;
                
                let na  = atan2(pos.x, pos.z) + dt * g_speed_scale * 0.6;
                let np  = float3(sin(na) * current_r, pos.y, cos(na) * current_r);
                let ny  = na * 57.2958 + 90.0;
                set_transform(world, e, np, float3(euler.x, ny, euler.z), scale);
            }
        };
    }

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

	let is_sprinting = is_action_down("sprint_key") && inputZ > 0.0; // Only sprint forward

	// Pass -1 for runClip if you don't have a run animation, or pass your run clip ID!
	update_player_controller(world, dt, g_player, g_player_idle_clip, g_player_walk_clip, inputX, inputZ, g_cam, is_sprinting, -1);
    update_third_person_camera(world, g_cam, g_player);

    // ── Physics toys keyboard shortcuts ──────────────────────────────────────
    let toy_sz = g_toy_size;
    let toy_h  = g_toy_drop_height;
    let p_pos = get_position(world, g_player); // Spawn toys above player

    if (is_action_pressed("spawn_cube_key"))
    {
        spawn_toy(world, false, float3(p_pos.x, toy_h, p_pos.z), toy_sz);
    }
    if (is_action_pressed("spawn_sphere_key"))
    {
        spawn_toy(world, true, float3(p_pos.x, toy_h, p_pos.z), toy_sz);
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

    // ── Global Keyboard shortcuts ────────────────────────────────────────────
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