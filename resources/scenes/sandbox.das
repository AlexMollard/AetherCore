require world
require renderer
require camera
require math
require systems/fox_system

[export]
def on_attach()
{
    // Lighting
    set_ambient(float3(0.08, 0.08, 0.12));
    set_sun(float3(-0.5, -1.0, -0.3), 2.0, float3(1.0, 0.95, 0.85));
    add_point_light(float3(0.0, 3.0, 0.0), float3(1.0, 0.8, 0.4), 5.0, 15.0);
    set_sky(float3(0.5, 0.7, 1.0), float3(0.1, 0.2, 0.6));

    // Camera
    let cam = create_orbit_camera(float3(0.0, 5.0, 10.0), float3(0.0, 0.0, 0.0), 60.0);
    set_main_camera(cam);

    // Scene entities
    spawn_foxes(20, 12.0);
}

[export]
def on_update()
{
}

[export]
def on_detach()
{
}
