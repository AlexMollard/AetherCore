options gen2;

// ── Game state machine ─────────────────────────────────────────────────────────
// A simple integer-based FSM.  States:
//   0 = Playing  (free roam, NPC AI active)
//   1 = Dialogue (dialogue UI open, player + NPC paused)
//   2 = Paused   (everything paused)

var g_game_state : int = 0;

[export]
def set_game_state(s : int)
{
    g_game_state = s;
}

[export]
def get_game_state() : int
{
    return g_game_state;
}

[export]
def is_state(s : int) : bool
{
    return g_game_state == s;
}
