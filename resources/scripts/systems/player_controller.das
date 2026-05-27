options gen2;

require world;
require input;
require game_components;
require state_machine;

// ── Player controller state ────────────────────────────────────────────────────

var g_player_entity  : uint  = 0u;
var g_move_speed     : float = 4.0;
var g_interact_range : float = 3.5;

[export]
def set_player_entity(e : uint)
{
    g_player_entity = e;
}

[export]
def get_player_entity() : uint
{
    return g_player_entity;
}

[export]
def update_player(world : World?; dt : float)
{
    if (!is_state(0)) { return; }
    if (g_player_entity == 0u) { return; }
    if (!has_tag(world, g_player_entity, get_tag_id("player"))) { return; }

    let pos = get_position(world, g_player_entity);
    var new_pos = pos;

    if (is_key_down(KEY_W)) { new_pos.z -= dt * g_move_speed; }
    if (is_key_down(KEY_S)) { new_pos.z += dt * g_move_speed; }
    if (is_key_down(KEY_A)) { new_pos.x -= dt * g_move_speed; }
    if (is_key_down(KEY_D)) { new_pos.x += dt * g_move_speed; }

    set_position(world, g_player_entity, new_pos);
}

[export]
def find_nearest_npc(world : World?) : uint
{
    let player_pos = get_position(world, g_player_entity);
    var nearest    : uint = 0u;
    var nearest_d  : float = g_interact_range;

    for_each_with_tag(world, get_tag_id("npc")) $(e : uint)
    {
        let npc_pos = get_position(world, e);
        let d = distance(npc_pos, player_pos);
        if (d < nearest_d)
        {
            nearest_d = d;
            nearest   = e;
        }
    };
    return nearest;
}
