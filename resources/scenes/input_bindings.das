options gen2;

require input;
require keycodes;

// ── Action binding ────────────────────────────────────────
// Maps a semantic action name (e.g. "jump") to one or two
// physical keys, fully rebindable at runtime.
struct InputAction
{
    name      : string;
    primary   : int;
    secondary : int;
}

var g_actions : array<InputAction>;

// ── Internal helpers ──────────────────────────────────────

def private find_action_idx(name : string) : int
{
    for (i in 0..length(g_actions))
    {
        if (g_actions[i].name == name) {
            return i;
        }
    }
    return -1;
}

// ── Public API ────────────────────────────────────────────

[export]
def register_action(name : string; primary : int; secondary : int)
{
    g_actions |> push(InputAction(name=name, primary=primary, secondary=secondary));
}

[export]
def is_action_pressed(name : string) : bool
{
    let idx = find_action_idx(name);
    if (idx < 0) { return false; }
    
    let action = g_actions[idx];
    if (is_key_pressed(action.primary)) { return true; }
    if (action.secondary > 0 && is_key_pressed(action.secondary)) { return true; }
    
    return false;
}

[export]
def is_action_down(name : string) : bool
{
    let idx = find_action_idx(name);
    if (idx < 0) { return false; }
    
    let action = g_actions[idx];
    if (is_key_down(action.primary)) { return true; }
    if (action.secondary > 0 && is_key_down(action.secondary)) { return true; }
    
    return false;
}

[export]
def is_action_released(name : string) : bool
{
    let idx = find_action_idx(name);
    if (idx < 0) { return false; }
    
    let action = g_actions[idx];
    if (is_key_released(action.primary)) { return true; }
    if (action.secondary > 0 && is_key_released(action.secondary)) { return true; }
    
    return false;
}

// ── Default sandbox bindings ──────────────────────────────

[export]
def init_default_bindings()
{
    register_action("toggle_rotate",  KEY_SPACE,      0);
    register_action("speed_up",       KEY_EQUAL,      0);
    register_action("speed_down",     KEY_MINUS,      0);
    register_action("toggle_manual",  KEY_T,          0);
    register_action("time_up",        KEY_I,          0);
    register_action("time_down",      KEY_J,          0);
    register_action("time_noon",      KEY_G,          0);
    register_action("time_midnight",  KEY_O,          0);
}