options gen2;

require world;
require math;
require state_machine;

// ── Per-NPC AI state (stored in parallel arrays indexed by entity id) ──────────

struct NpcAiState
{
    idle_timer   : float
    patrol_index : int
    waypoints    : array<float3>
}

var g_npc_ai : table<uint, NpcAiState>;

// ── API ────────────────────────────────────────────────────────────────────────

[export]
def init_npc_ai(world : World?)
{
    g_npc_ai |> clear();
}

[export]
def set_npc_waypoints(world : World?; npc_id : uint; wps : array<float3>)
{
    var state = NpcAiState();
    state.idle_timer = 0.0;
    state.patrol_index = 0;
    state.waypoints = wps;
    g_npc_ai[npc_id] = state;
}

[export]
def update_npcs(world : World?; dt : float)
{
    if (!is_state(0)) { return; }  // only update in Playing state

    for_each_with_tag(world, get_tag_id("npc")) $(e : uint)
    {
        if (e in g_npc_ai)
        {
            let state = g_npc_ai[e];
            update_npc_ai(world, e, state, dt);
        }
    };
}

def private update_npc_ai(world : World?; e : uint; var state : NpcAiState; dt : float)
{
    if (length(state.waypoints) < 2)
    {
        // No patrol route - idle in place
        return;
    }

    state.idle_timer = max(state.idle_timer - dt, 0.0);

    if (state.idle_timer > 0.0) { return; }

    let target = state.waypoints[state.patrol_index];
    let pos    = get_position(world, e);
    let dir    = target - pos;
    let dist   = length(dir);

    if (dist < 0.5)
    {
        // Reached waypoint - move to next
        state.patrol_index = (state.patrol_index + 1) % length(state.waypoints);
        state.idle_timer = 2.0 + float(rand() % 3);
    }
    else
    {
        // Move toward waypoint
        let speed   = 2.0;
        let step    = dir / dist * speed * dt;
        let new_pos = pos + step;
        set_position(world, e, new_pos);

        // Face movement direction
        let angle = atan2(dir.x, dir.z) * 57.2958;
        let euler = get_euler(world, e);
        set_transform(world, e, new_pos, float3(euler.x, angle, euler.z), get_scale(world, e));
    }

    g_npc_ai[e] = state;
}
