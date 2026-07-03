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
require systems/character_controller;
require systems/third_person_camera;

// ════════════════════════════════════════════════════════════════════════════
//  AetherCore Test Gallery
//  A clean, zoned scene for evaluating rendering features in isolation. Nothing
//  overlaps, and no light sits inside geometry, so each effect reads cleanly.
//
//  Layout (top-down, +X right, +Z toward the start camera):
//
//        -Z (far)
//     A ───────── B          A  Sun-shadow calibration  (NW)
//     │    E      │          B  Ambient-occlusion crease (NE)
//     │   (P)     │          C  Single point-light shadow (SW)
//     C ───────── D          D  3-colour light overlap    (SE)
//        +Z (near)           E  Spot light on a wall      (far centre)
//                            P  Player + foxes (centre)
//
//  Neutral white surfaces (plane floor, plane walls, spheres) are the light/AO
//  RECEIVERS so colour and darkening read truthfully. Cubes (rainbow vertex
//  colours) are used as shadow CASTERS - the shadow they throw is neutral, and
//  their colour makes them easy to pick out.
//
//  Controls: WASD move, Shift sprint, LMB-drag orbit, scroll zoom.
//            1 drop cube, 2 drop sphere, 3 sphere rain, 4 box tower,
//            B bounce test, C clear toys.
// ════════════════════════════════════════════════════════════════════════════

// ── Cached primitive meshes ─────────────────────────────────────────────────
var g_cube   : uint = 0u;
var g_sphere : uint = 0u;
var g_plane  : uint = 0u;

// ── Tags ────────────────────────────────────────────────────────────────────
var g_ground_tag : uint = 0u;
var g_fox_tag    : uint = 0u;
var g_toy_tag    : uint = 0u;

// ── Entities / state ─────────────────────────────────────────────────────────
var g_player           : uint = 0u;
var g_cam              : uint = 0u;
var g_plasma           : uint = 0u;
var g_player_idle_clip : int  = 0;
var g_player_walk_clip : int  = 1;
var g_time             : float = 0.0;

// ── Physics-toy tuning ───────────────────────────────────────────────────────
var g_toy_size  : float = 0.6;
var g_toy_drop  : float = 20.0;

// ── Tag-enumeration scratch (globals, matching the block-capture idiom the
//    for_each_with_tag callback relies on) ─────────────────────────────────────
var g_enum_count  : int  = 0;
var g_enum_target : int  = -1;
var g_enum_result : uint = 0u;

// ════════════════════════════════════════════════════════════════════════════
//  Construction helpers
// ════════════════════════════════════════════════════════════════════════════

// Static box: cube mesh + matching static collider. Used for pillars, steps and
// shadow casters. `size` is the full world-space extent.
def static_box(world : World?; pos : float3; euler : float3; size : float3) : uint
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, pos, euler, size);
    add_mesh(world, e, g_cube);
    add_box_body(world, e, size * 0.5, false);
    add_tag(world, e, g_ground_tag);
    return e;
}

// Neutral white decorative sphere (visual only). `diameter` = world size.
// Closed mesh, so it both receives and casts shadows cleanly.
def decor_sphere(world : World?; pos : float3; diameter : float) : uint
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, pos, float3(0.0, 0.0, 0.0), float3(diameter, diameter, diameter));
    add_mesh(world, e, g_sphere);
    return e;
}

// Neutral white wall from the plane primitive (single-sided, faces +Z before
// rotation). `euler` orients it; `w`/`h` are width/height.
def wall(world : World?; pos : float3; euler : float3; w : float; h : float) : uint
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, pos, euler, float3(w, h, 1.0));
    add_mesh(world, e, g_plane);
    return e;
}

// Big neutral floor: a flat plane for clean shading + an invisible box collider
// whose top face sits exactly at y = 0 so toys rest on the visible surface.
def build_floor(world : World?)
{
    let visual = entity_create(world);
    add_transform(world, visual);
    // Plane primitive lies in XY facing +Z; rotate -90 about X to lay it flat,
    // normal pointing up. Local (X,Y) -> world (X,Z).
    set_transform(world, visual, float3(0.0, 0.0, 0.0), float3(-90.0, 0.0, 0.0), float3(120.0, 120.0, 1.0));
    add_mesh(world, visual, g_plane);

    let collider = entity_create(world);
    add_transform(world, collider);
    set_transform(world, collider, float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
    add_box_body(world, collider, float3(60.0, 1.0, 60.0), false);
    add_tag(world, collider, g_ground_tag);
}

// ════════════════════════════════════════════════════════════════════════════
//  Physics toys
// ════════════════════════════════════════════════════════════════════════════

def spawn_toy(world : World?; is_sphere : bool; pos : float3; size : float)
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, pos, float3(0.0, 0.0, 0.0), float3(size, size, size));
    if (is_sphere)
    {
        add_mesh(world, e, g_sphere);
        add_sphere_body(world, e, size * 0.5, true);
    }
    else
    {
        add_mesh(world, e, g_cube);
        add_box_body(world, e, float3(size * 0.5, size * 0.5, size * 0.5), true);
    }
    add_tag(world, e, g_toy_tag);
}

def count_tagged(world : World?; tag : uint) : int
{
    g_enum_count = 0;
    for_each_with_tag(world, tag) $(e : uint)
    {
        g_enum_count += 1;
    };
    return g_enum_count;
}

def find_nth_tagged(world : World?; tag : uint; target : int) : uint
{
    g_enum_count = 0;
    g_enum_target = target;
    g_enum_result = 0u;
    for_each_with_tag(world, tag) $(e : uint)
    {
        if (g_enum_count == g_enum_target) { g_enum_result = e; }
        g_enum_count += 1;
    };
    return g_enum_result;
}

def clear_toys(world : World?)
{
    var k = count_tagged(world, g_toy_tag);
    while (k > 0)
    {
        k -= 1;
        let e = find_nth_tagged(world, g_toy_tag, k);
        if (e != 0u && entity_valid(e))
        {
            entity_destroy(world, e);
        }
    }
}

def spawn_tower(world : World?; base : float3)
{
    let sz = g_toy_size;
    for (i in 0..8)
    {
        let y = 1.0 + float(i) * sz * 1.05;
        let jx = (float(i % 2) - 0.5) * 0.04;
        spawn_toy(world, false, float3(base.x + jx, y, base.z), sz);
    }
}

def spawn_rain(world : World?; base : float3)
{
    let sz = g_toy_size;
    for (i in 0..8)
    {
        let ang = float(i) * 0.7854;
        let x   = base.x + cos(ang) * 4.0;
        let z   = base.z + sin(ang) * 4.0;
        spawn_toy(world, true, float3(x, g_toy_drop, z), sz);
    }
}

def spawn_bounce(world : World?; base : float3)
{
    let sz = g_toy_size;
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, float3(base.x, 1.0, base.z), float3(0.0, 0.0, 0.0), float3(sz, sz, sz));
    add_mesh(world, e, g_cube);
    add_box_body(world, e, float3(sz * 0.5, sz * 0.5, sz * 0.5), true);
    add_tag(world, e, g_toy_tag);
    set_linear_velocity(world, e, 0.0, 20.0, 0.0);
}

// ════════════════════════════════════════════════════════════════════════════
//  Models
// ════════════════════════════════════════════════════════════════════════════

def spawn_fox(world : World?; pos : float3; yaw : float; desync : float)
{
    let fox = entity_create(world);
    add_transform(world, fox);
    set_transform(world, fox, pos, float3(0.0, yaw, 0.0), float3(0.05, 0.05, 0.05));
    load_model(world, fox, "assets://models/Fox/Fox.mesh");
    set_animation(world, fox, 2);          // Run
    set_playback_speed(world, fox, 0.8);
    set_anim_time(world, fox, desync);     // stagger the herd
    add_tag(world, fox, g_fox_tag);
}

// ════════════════════════════════════════════════════════════════════════════
//  Test zones
// ════════════════════════════════════════════════════════════════════════════

// Zone A (NW): directional-shadow calibration. Isolated shapes throwing long,
// clean sun shadows across the neutral floor.
def build_zone_sun_shadows(world : World?)
{
    static_box(world, float3(-46.0, 5.0, -46.0), float3(0.0, 0.0, 0.0), float3(3.0, 10.0, 3.0));  // tall pillar
    static_box(world, float3(-46.0, 1.5, -34.0), float3(0.0, 0.0, 0.0), float3(4.0, 3.0, 4.0));   // stub
    static_box(world, float3(-34.0, 2.0, -46.0), float3(0.0, 30.0, 0.0), float3(3.0, 4.0, 6.0));  // rotated slab

    // Sphere resting on a low pedestal - shows both a cast shadow and its own
    // contact darkening on the pedestal top.
    static_box(world, float3(-34.0, 0.5, -34.0), float3(0.0, 0.0, 0.0), float3(6.0, 1.0, 6.0));
    decor_sphere(world, float3(-34.0, 3.0, -34.0), 4.0);
}

// Zone B (NE): ambient-occlusion crease test. A neutral inside corner and a
// huddle of spheres - AO should darken every crease and contact point. Sun +
// ambient only (no local light) so occlusion is the whole story.
def build_zone_ao(world : World?)
{
    wall(world, float3(40.0, 5.0, -48.0), float3(0.0,  0.0, 0.0), 18.0, 10.0); // back wall (+Z)
    wall(world, float3(31.0, 5.0, -40.0), float3(0.0, 90.0, 0.0), 16.0, 10.0); // side wall (+X)

    // Spheres nestled into the corner (contact with two walls + floor).
    decor_sphere(world, float3(35.0, 2.0, -44.0), 4.0);
    decor_sphere(world, float3(33.2, 1.5, -45.2), 3.0);
    decor_sphere(world, float3(35.5, 1.5, -41.5), 3.0);

    // Free huddle of touching spheres (sphere-sphere + sphere-floor AO).
    decor_sphere(world, float3(45.0, 1.5, -35.0), 3.0);
    decor_sphere(world, float3(47.8, 1.5, -35.0), 3.0);
    decor_sphere(world, float3(46.4, 1.5, -37.4), 3.0);
}

// Zone C (SW): a single point light so cube-face point shadows are unambiguous.
// A ring of separated spheres each throws one clean radial shadow.
def build_zone_point_shadow(world : World?)
{
    add_point_light(float3(-40.0, 13.0, 40.0), float3(1.0, 0.9, 0.8), 22.0, 30.0, true);

    static_box(world, float3(-40.0, 3.0, 40.0), float3(0.0, 0.0, 0.0), float3(2.5, 6.0, 2.5)); // centre pillar
    for (i in 0..6)
    {
        let a  = float(i) * 1.0472; // 60 deg
        let sx = -40.0 + cos(a) * 9.0;
        let sz =  40.0 + sin(a) * 9.0;
        decor_sphere(world, float3(sx, 1.5, sz), 3.0);
    }
}

// Zone D (SE): deliberate multi-light stress test (now that the shadow budget
// fits every light). Three coloured point lights, spaced apart, over one shared
// cluster -> overlapping coloured shadows.
def build_zone_light_overlap(world : World?)
{
    add_point_light(float3(34.0, 11.0, 40.0), float3(1.0, 0.2, 0.2), 18.0, 26.0, true); // red
    add_point_light(float3(46.0, 11.0, 40.0), float3(0.2, 1.0, 0.3), 18.0, 26.0, true); // green
    add_point_light(float3(40.0, 11.0, 47.0), float3(0.3, 0.4, 1.0), 18.0, 26.0, true); // blue

    static_box(world, float3(40.0, 3.5, 40.0), float3(0.0, 0.0, 0.0), float3(2.5, 7.0, 2.5));
    for (i in 0..4)
    {
        let a = float(i) * 1.5708; // 90 deg
        decor_sphere(world, float3(40.0 + cos(a) * 6.0, 1.5, 40.0 + sin(a) * 6.0), 3.0);
    }
}

// Zone E (far centre): spot light projecting a caster's shadow onto a wall.
// Wall faces +Z toward the start camera.
def build_zone_spot(world : World?)
{
    wall(world, float3(0.0, 6.0, -54.0), float3(0.0, 0.0, 0.0), 20.0, 12.0);

    // Spot in front of the wall, aimed at it and slightly down.
    add_spot_light(float3(0.0, 13.0, -44.0), float3(0.8, 0.9, 1.0), 30.0, 45.0, float3(0.0, -0.15, -1.0), 0.35, 0.55, true);

    static_box(world, float3(0.0, 3.0, -49.0), float3(0.0, 0.0, 0.0), float3(3.0, 6.0, 3.0)); // caster
    decor_sphere(world, float3(5.0, 2.0, -49.0), 4.0);                                        // round caster
}

// ════════════════════════════════════════════════════════════════════════════
//  on_attach
// ════════════════════════════════════════════════════════════════════════════

[export]
def on_attach(world : World?)
{
    g_cube   = create_mesh(world, "cube");
    g_sphere = create_mesh(world, "sphere");
    g_plane  = create_mesh(world, "plane");
    g_time   = 0.0;

    g_ground_tag = create_tag("ground");
    g_fox_tag    = create_tag("fox");
    g_toy_tag    = create_tag("phys_toy");

    // ── Global lighting ──────────────────────────────────────────────────────
    // Low neutral ambient so AO and shadows read but nothing goes fully black.
    clear_lights();
    set_ambient(float3(0.06, 0.07, 0.09));

    // Warm key sun at a raking angle for long, legible shadows.
    set_sun(float3(0.35, -0.8, 0.45), 2.2, float3(1.0, 0.95, 0.85));
    set_sky(float3(0.5, 0.6, 0.75), float3(0.15, 0.2, 0.35));
    set_sky_void(float3(0.02, 0.03, 0.05));

    // ── World ────────────────────────────────────────────────────────────────
    build_floor(world);
    build_zone_sun_shadows(world);
    build_zone_ao(world);
    build_zone_point_shadow(world);
    build_zone_light_overlap(world);
    build_zone_spot(world);

    // ── Effects showcase: one isolated plasma orb floating high overhead ──────
    g_plasma = entity_create(world);
    add_transform(world, g_plasma);
    set_transform(world, g_plasma, float3(-20.0, 20.0, -18.0), float3(0.0, 0.0, 0.0), float3(3.0, 3.0, 3.0));
    add_mesh(world, g_plasma, g_sphere);
    set_entity_effect(world, g_plasma, "plasma");
    set_effect_color(world, g_plasma, 0.9, 0.4, 1.0);
    set_effect_speed(world, g_plasma, 1.0);
    set_effect_intensity(world, g_plasma, 1.8);

    // ── Foxes (animation test - Run in place, staggered) ─────────────────────
    spawn_fox(world, float3(14.0, 0.0, 15.0),  40.0, 0.0);
    spawn_fox(world, float3(20.0, 0.0,  8.0), -60.0, 0.3);
    spawn_fox(world, float3(-16.0, 0.0, 14.0), 20.0, 0.6);

    // ── Player (Human, third-person controlled) at the centre ────────────────
    g_player = entity_create(world);
    add_transform(world, g_player);
    set_transform(world, g_player, float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.05, 0.05, 0.05));
    load_model(world, g_player, "assets://models/Human/Human.mesh");
    g_player_idle_clip = get_current_animation(world, g_player);
    set_playback_speed(world, g_player, 1.0);
    add_tag(world, g_player, create_tag("player"));

    g_player_walk_clip = add_animation(world, g_player, "assets://animations/Walking_mixamo.com.anim", true);
    if (g_player_walk_clip >= 0)
    {
        compile_animations(world, g_player);
    }

    // ── Camera & input ───────────────────────────────────────────────────────
    g_cam = create_orbit_camera(float3(0.0, 30.0, 50.0), float3(0.0, 3.0, 0.0), 65.0);
    set_main_camera(g_cam);

    init_default_bindings();
    register_action("sprint_key",   KEY_LEFT_SHIFT, 0);
    register_action("spawn_cube",   KEY_1, 0);
    register_action("spawn_sphere", KEY_2, 0);
    register_action("spawn_rain",   KEY_3, 0);
    register_action("spawn_tower",  KEY_4, 0);
    register_action("bounce_test",  KEY_B, 0);
    register_action("clear_toys",   KEY_C, 0);
}

// ════════════════════════════════════════════════════════════════════════════
//  on_update
// ════════════════════════════════════════════════════════════════════════════

[export]
def on_update(world : World?)
{
    let dt = get_delta_time();
    g_time += dt;

    // ── Player movement + third-person camera ────────────────────────────────
    var inputX = 0.0; var inputZ = 0.0;
    if (is_action_down("move_forward"))  { inputZ += 1.0; }
    if (is_action_down("move_backward")) { inputZ -= 1.0; }
    if (is_action_down("move_left"))     { inputX -= 1.0; }
    if (is_action_down("move_right"))    { inputX += 1.0; }

    let is_sprinting = is_action_down("sprint_key") && inputZ > 0.0;
    update_player_controller(world, dt, g_player, g_player_idle_clip, g_player_walk_clip, inputX, inputZ, g_cam, is_sprinting, -1);
    update_third_person_camera(world, g_cam, g_player);

    // ── Gentle plasma bob (isolated overhead, never enters geometry) ──────────
    if (entity_valid(g_plasma))
    {
        var p = get_position(world, g_plasma);
        p.y = 20.0 + sin(g_time * 0.8) * 1.5;
        set_position(world, g_plasma, p);
    }

    // ── Physics toys, spawned above the player ───────────────────────────────
    let pp = get_position(world, g_player);
    if (is_action_pressed("spawn_cube"))   { spawn_toy(world, false, float3(pp.x, g_toy_drop, pp.z), g_toy_size); }
    if (is_action_pressed("spawn_sphere")) { spawn_toy(world, true,  float3(pp.x, g_toy_drop, pp.z), g_toy_size); }
    if (is_action_pressed("spawn_rain"))   { spawn_rain(world, pp); }
    if (is_action_pressed("spawn_tower"))  { spawn_tower(world, pp); }
    if (is_action_pressed("bounce_test"))  { spawn_bounce(world, pp); }
    if (is_action_pressed("clear_toys"))   { clear_toys(world); }
}

// ════════════════════════════════════════════════════════════════════════════
//  on_detach
// ════════════════════════════════════════════════════════════════════════════

[export]
def on_detach(world : World?)
{
}
