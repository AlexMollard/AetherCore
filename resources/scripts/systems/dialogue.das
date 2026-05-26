options gen2;

require world;
require ui;
require game_components;
require data_loader;
require state_machine;

// ── Dialogue UI state ──────────────────────────────────────────────────────────
// These handles are created once in on_attach and reused each frame.

var g_dlg_panel   : uint = 0u;
var g_dlg_text    : uint = 0u;
var g_dlg_responses : array<uint>;
var g_dlg_speaker : uint = 0u;

var g_dialogues    : table<string, DialogueNode>;  // loaded at init
var g_dlg_active   : bool  = false;
var g_dlg_entity   : uint  = 0u;                   // NPC entity with dialogue_state

// ── Init / cleanup ─────────────────────────────────────────────────────────────

[export]
def init_dialogue_system(world : World?)
{
    let screen_w = float(get_ui_screen_width());
    let screen_h = float(get_ui_screen_height());
    let panel_w  = 500.0;
    let panel_h  = 280.0;
    let px       = (screen_w - panel_w) * 0.5;
    let py       = screen_h - panel_h - 40.0;

    g_dlg_panel = create_ui_panel(world, px, py, panel_w, panel_h, "Dialogue");
    set_ui_visible(world, g_dlg_panel, false);

    g_dlg_speaker = create_ui_label(world, px + 16.0, py + 36.0, panel_w - 32.0, 24.0, "");
    set_ui_font_size(world, g_dlg_speaker, 16.0);
    set_ui_text_color(world, g_dlg_speaker, float4(1.0, 0.85, 0.5, 1.0));
    add_ui_child(world, g_dlg_panel, g_dlg_speaker);

    g_dlg_text = create_ui_label(world, px + 16.0, py + 64.0, panel_w - 32.0, 80.0, "");
    set_ui_font_size(world, g_dlg_text, 14.0);
    set_ui_text_color(world, g_dlg_text, float4(1.0, 1.0, 1.0, 1.0));
    add_ui_child(world, g_dlg_panel, g_dlg_text);

    // Pre-create a pool of response buttons (up to 6)
    for (i in 0..6)
    {
        let btn = create_ui_button(world, px + 16.0, py + 160.0 + float(i) * 36.0, panel_w - 32.0, 30.0, "");
        set_ui_visible(world, btn, false);
        add_ui_child(world, g_dlg_panel, btn);
        g_dlg_responses |> push(btn);
    }
}

[export]
def load_dialogues_from(path : string)
{
    g_dialogues = load_dialogues(path);
}

[export]
def shutdown_dialogue(world : World?)
{
    for (h in g_dlg_responses)
    {
        entity_destroy(world, h);
    }
    g_dlg_responses |> clear();
    entity_destroy(world, g_dlg_text);
    g_dlg_text = 0u;
    entity_destroy(world, g_dlg_speaker);
    g_dlg_speaker = 0u;
    entity_destroy(world, g_dlg_panel);
    g_dlg_panel = 0u;
}

// ── Start a dialogue with an entity ────────────────────────────────────────────

[export]
def start_dialogue(world : World?; npc_entity : uint)
{
    let dlg_id = get_npc_dialogue_id(world, npc_entity);
    if (length(dlg_id) == 0) { return; }
    if (!(dlg_id in g_dialogues)) { return; }

    g_dlg_active = true;
    g_dlg_entity = npc_entity;

    add_dialogue_state(world, npc_entity);
    set_dialogue_state_npc_id(world, npc_entity, get_npc_npc_id(world, npc_entity));
    set_dialogue_state_current_node_id(world, npc_entity, dlg_id);

    show_dialogue_node(world, npc_entity);
    set_game_state(1);
}

def private show_dialogue_node(world : World?; npc_entity : uint)
{
    let node_id = get_dialogue_state_current_node_id(world, npc_entity);
    let node    = g_dialogues[node_id];
    if (node == null) { return; }

    let speaker_name = get_npc_display_name(world, npc_entity);
    set_ui_text(world, g_dlg_speaker, speaker_name);
    set_ui_text(world, g_dlg_text, node.text);
    set_ui_visible(world, g_dlg_panel, true);

    // Hide all response buttons first
    for (i in 0..6)
    {
        set_ui_visible(world, g_dlg_responses[i], false);
    }

    // Show response buttons for this node
    let count = min(length(node.responses), 6);
    for (i in 0..count)
    {
        let btn = g_dlg_responses[i];
        set_ui_text(world, btn, node.responses[i].text);
        set_ui_visible(world, btn, true);
    }

    // If no responses, add an auto "Close" button at index 0
    if (count == 0)
    {
        set_ui_text(world, g_dlg_responses[0], "(Close)");
        set_ui_visible(world, g_dlg_responses[0], true);
    }
}

// ── Update dialogue (returns true while dialogue is active) ────────────────────

[export]
def update_dialogue(world : World?) : bool
{
    if (!g_dlg_active) { return false; }
    if (!has_dialogue_state(world, g_dlg_entity))
    {
        g_dlg_active = false;
        return false;
    }

    let node_id = get_dialogue_state_current_node_id(world, g_dlg_entity);
    let node    = g_dialogues[node_id];
    if (node == null)
    {
        end_dialogue(world);
        return false;
    }

    let count = min(length(node.responses), 6);

    // Check each response button
    for (i in 0..count)
    {
        if (g_dlg_responses[i] != 0u && get_ui_clicked(world, g_dlg_responses[i]))
        {
            let next_id = node.responses[i].next_node;
            if (length(next_id) == 0)
            {
                end_dialogue(world);
                return false;
            }
            set_dialogue_state_current_node_id(world, g_dlg_entity, next_id);
            show_dialogue_node(world, g_dlg_entity);
            return true;
        }
    }

    return true;
}

[export]
def end_dialogue(world : World?)
{
    g_dlg_active = false;
    remove_dialogue_state(world, g_dlg_entity);
    set_ui_visible(world, g_dlg_panel, false);
    g_dlg_entity = 0u;
    set_game_state(0);
}
