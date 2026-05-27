options gen2;

require world;

[export]
def spawn_player(world : World?) : uint
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.05, 0.05, 0.05));
    add_tag(world, e, create_tag("player"));
    add_health(world, e);
    load_model(world, e, "assets://models/Human/Human.mesh");
    return e;
}
