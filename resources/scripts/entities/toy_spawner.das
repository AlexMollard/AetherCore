options gen2;

require world;
require physics;
require input;
require input_bindings;
require keycodes;
require math;

// ── Toy spawner entity script ────────────────────────────────────────────────
// The physics-toy hotkeys, moved out of the old sandbox.das god-script onto a
// plain scene entity ("Toy Spawner" in sandbox.scene.toml):
//   1 drop cube   2 drop sphere   3 sphere rain   4 box tower
//   B bounce test C clear toys
// Toys drop over this entity's position, so move the spawner to move the
// drop zone. Painted spheres dedup in the material registry: any number of
// cyan spheres consumes exactly one material slot.

var g_cube     : uint = 0u;
var g_sphere   : uint = 0u;
var g_toy_tag  : uint = 0u;
var g_toy_size : float = 0.6;
var g_toy_drop : float = 20.0;

def spawn_toy(world : World?; is_sphere : bool; pos : float3; size : float)
{
    let e = entity_create(world);
    set_name(world, e, is_sphere ? "Toy Sphere" : "Toy Cube");
    add_transform(world, e);
    set_transform(world, e, pos, float3(0.0, 0.0, 0.0), float3(size, size, size));
    if (is_sphere)
    {
        add_mesh(world, e, g_sphere);
        set_material_color(world, e, float3(0.2, 0.75, 0.9));
        add_sphere_body(world, e, size * 0.5, true);
    }
    else
    {
        add_mesh(world, e, g_cube);
        add_box_body(world, e, float3(size * 0.5, size * 0.5, size * 0.5), true);
    }
    add_tag(world, e, g_toy_tag);
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

def clear_all_toys(world : World?)
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

[export]
def on_entity_attach(world : World?; self : uint)
{
    g_cube    = create_mesh(world, "cube");
    g_sphere  = create_mesh(world, "sphere");
    g_toy_tag = create_tag("phys_toy");

    register_action("spawn_cube",   KEY_1, 0);
    register_action("spawn_sphere", KEY_2, 0);
    register_action("spawn_rain",   KEY_3, 0);
    register_action("spawn_tower",  KEY_4, 0);
    register_action("bounce_test",  KEY_B, 0);
    register_action("clear_toys",   KEY_C, 0);
}

[export]
def on_entity_update(world : World?; self : uint; dt : float)
{
    let pp = get_position(world, self);
    if (is_action_pressed("spawn_cube"))   { spawn_toy(world, false, float3(pp.x, g_toy_drop, pp.z), g_toy_size); }
    if (is_action_pressed("spawn_sphere")) { spawn_toy(world, true,  float3(pp.x, g_toy_drop, pp.z), g_toy_size); }
    if (is_action_pressed("spawn_rain"))   { spawn_rain(world, pp); }
    if (is_action_pressed("spawn_tower"))  { spawn_tower(world, pp); }
    if (is_action_pressed("bounce_test"))  { spawn_bounce(world, pp); }
    if (is_action_pressed("clear_toys"))   { clear_all_toys(world); }
}
