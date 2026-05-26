options gen2;

require world;
require renderer;
require camera;
require input;
require input_bindings;
require ui;
require math;
require game_components;
require systems/data_loader;
require player;
require npc;
require systems/state_machine;
require systems/dialogue;
require systems/npc_ai;
require systems/player_controller;

// ── Scene state ────────────────────────────────────────────────────────────────

var g_npcs             : array<uint>;
var g_cube_mesh        : uint = 0u;
var g_interact_prompt  : uint = 0u;
var g_cam              : uint = 0u;

// ── on_attach ──────────────────────────────────────────────────────────────────

[export]
def on_attach(world : World?)
{
    g_npcs |> clear();
    g_cube_mesh = create_mesh(world, "cube");

    // ── Lighting ──────────────────────────────────────────────────────────
    clear_lights();
    set_ambient(float3(0.08, 0.08, 0.10));
    set_sun(float3(-0.3, -0.8, -0.5), 3.0, float3(1.0, 0.95, 0.85));
    set_sky(float3(0.35, 0.55, 0.90), float3(0.06, 0.12, 0.40));

    // ── Camera ────────────────────────────────────────────────────────────
    g_cam = create_orbit_camera(float3(0.0, 8.0, 20.0), float3(0.0, 1.0, 0.0), 30.0);
    set_main_camera(g_cam);

    // ── Ground ────────────────────────────────────────────────────────────
    let floor = entity_create(world);
    add_transform(world, floor);
    set_transform(world, floor, float3(0.0, -0.5, 0.0), float3(0.0, 0.0, 0.0), float3(40.0, 1.0, 40.0));
    add_mesh(world, floor, g_cube_mesh);
    add_box_body(world, floor, float3(20.0, 0.5, 20.0), false);

    // ── Player ────────────────────────────────────────────────────────────
    let player = spawn_player(world);
    set_player_entity(player);

    // ── NPCs ──────────────────────────────────────────────────────────────
    let npc_data = load_npcs("data://npcs.json");
    for (npc in npc_data)
    {
        let e = spawn_npc(world, npc);
        g_npcs |> push(e);

        // Give NPCs a simple patrol route
        let pos = npc.position;
        let wps = array<float3>(
            pos + float3(3.0, 0.0, 0.0),
            pos + float3(-3.0, 0.0, 0.0)
        );
        set_npc_waypoints(world, e, wps);
    }

    // ── Dialogue system ───────────────────────────────────────────────────
    init_dialogue_system(world);
    load_dialogues_from("data://dialogues.json");
    init_npc_ai(world);

    // ── Interaction prompt (hidden by default) ────────────────────────────
    g_interact_prompt = create_ui_label(world, 16.0, 16.0, 400.0, 28.0, "");
    set_ui_font_size(world, g_interact_prompt, 14.0);
    set_ui_text_color(world, g_interact_prompt, float4(1.0, 1.0, 0.7, 1.0));
    set_ui_visible(world, g_interact_prompt, false);

    // ── Input bindings ────────────────────────────────────────────────────
    register_action("interact", KEY_E, 0);
}

// ── on_update ──────────────────────────────────────────────────────────────────

[export]
def on_update(world : World?)
{
    let dt = get_delta_time();

    // ── State dispatch ────────────────────────────────────────────────────
    let state = get_game_state();

    if (state == 0)
    {
        // Playing
        update_player(world, dt);
        update_npcs(world, dt);

        // Show interaction prompt when near an NPC
        let nearest = find_nearest_npc(world);
        if (nearest != 0u)
        {
            let npc_name = get_npc_display_name(world, nearest);
            set_ui_text(world, g_interact_prompt, "Press E to talk to {npc_name}");
            set_ui_visible(world, g_interact_prompt, true);

            if (is_action_pressed("interact"))
            {
                start_dialogue(world, nearest);
            }
        }
        else
        {
            set_ui_visible(world, g_interact_prompt, false);
        }
    }
    elif (state == 1)
    {
        // Dialogue
        let still_active = update_dialogue(world);
        if (!still_active)
        {
            set_game_state(0);
        }
    }
    // state 2 (Paused) -- no update
}

// ── on_detach ──────────────────────────────────────────────────────────────────

[export]
def on_detach(world : World?)
{
    entity_destroy(world, g_interact_prompt);
    g_interact_prompt = 0u;
    shutdown_dialogue(world);
}
