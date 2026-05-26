options gen2;

require world;
require math;

[export]
def spawn_npc(world : World?; npc : NpcData)
{
    let e = entity_create(world);
    add_transform(world, e);
    set_transform(world, e, npc.position, npc.rotation, npc.scale);
    add_npc(world, e);
    add_health(world, e);
    load_model(world, e, npc.model);

    set_npc_npc_id(world, e, npc.id);
    set_npc_display_name(world, e, npc.name);
    set_npc_dialogue_id(world, e, npc.dialogue_id);

    return e;
}
