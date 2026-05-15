require world
require math

// Spawn `count` foxes arranged in a circle of the given radius.
// Called from sandbox.das on_attach.
def spawn_foxes(count : int; radius : float)
{
    let step = 6.2832 / float(count);
    for i in range(count)
    {
        let angle  = float(i) * step;
        let pos    = float3(cos(angle) * radius, 0.0, sin(angle) * radius);
        let yaw    = angle * 57.2958 + 90.0;
        let fox    = spawn();
        set_transform(fox, pos, float3(0.0, yaw, 0.0), float3(0.05, 0.05, 0.05));
        load_model(fox, "assets://models/Fox/Fox.mesh");
    }
}
