options gen2;

require world;
require animation;
require camera;
require input;
require math;
require systems/character_controller;
require systems/third_person_camera;

// ── Player entity script ─────────────────────────────────────────────────────
// The player is ordinary SCENE CONTENT now: a bare entity carrying this
// script (see sandbox.das first-boot block). On the first play tick the
// attach builds the model/animations/camera; Stop restores the scene record
// and the next play re-attaches. Movement actions (move_*/sprint_key) are
// registered globally by sandbox.das.
//
// Single-instance script: the globals below assume ONE player entity.

var g_cam       : uint = 0u;
var g_idle_clip : int  = 0;
var g_walk_clip : int  = -1;

[export]
def on_entity_attach(world : World?; self : uint)
{
    // Build only when needed: a Stop-restore hands back the fully built
    // player (model children + baked animation clips survive in the shared
    // model cache), so rebuilding would stack a second model.
    if (get_animation_count(world, self) == 0)
    {
        load_model(world, self, "assets://models/Human/Human.mesh");
        set_playback_speed(world, self, 1.0);
        // The walk clip bakes into the model's SHARED animation database -
        // only append it the first time this session, or every play/stop
        // cycle would grow the clip list.
        if (get_animation_count(world, self) < 2)
        {
            let walk = add_animation(world, self, "assets://animations/Walking_mixamo.com.anim", true);
            if (walk >= 0)
            {
                compile_animations(world, self);
            }
        }
    }

    // Clip layout after the bake: 0 = model idle, 1 = appended walk.
    g_idle_clip = 0;
    g_walk_clip = get_animation_count(world, self) > 1 ? 1 : -1;

    // One orbit camera per session; every attach reclaims it as main (the
    // editor's viewport hands the view back to it on Play).
    if (g_cam == 0u)
    {
        g_cam = create_orbit_camera(float3(0.0, 30.0, 50.0), float3(0.0, 3.0, 0.0), 65.0);
    }
    set_main_camera(g_cam);
}

[export]
def on_entity_update(world : World?; self : uint; dt : float)
{
    var inputX = 0.0;
    var inputZ = 0.0;
    if (is_action_down("move_forward"))  { inputZ += 1.0; }
    if (is_action_down("move_backward")) { inputZ -= 1.0; }
    if (is_action_down("move_left"))     { inputX -= 1.0; }
    if (is_action_down("move_right"))    { inputX += 1.0; }

    let is_sprinting = is_action_down("sprint_key") && inputZ > 0.0;
    update_player_controller(world, dt, self, g_idle_clip, g_walk_clip, inputX, inputZ, g_cam, is_sprinting, -1);
    update_third_person_camera(world, g_cam, self);
}
