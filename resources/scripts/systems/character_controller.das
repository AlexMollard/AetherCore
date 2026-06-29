options gen2;

require world;
require animation;
require camera;
require math;

// ── Controller Tuning ────────────────────────────────────────────────────────
var g_walk_speed     : float = 10.0;
var g_sprint_speed   : float = 15.0;
var g_acceleration   : float = 50.0; // How fast we reach top speed
var g_deceleration   : float = 50.0; // How fast we stop when input is released
var g_rotation_speed : float = 14.0; // How fast the model turns to face movement
var g_anim_blend_spd : float = 6.0;
var g_current_clip   : int = -1;

// ── Internal State ───────────────────────────────────────────────────────────
// We store velocity and yaw globally to give the controller momentum.
var g_player_vel_x   : float = 0.0;
var g_player_vel_z   : float = 0.0;
var g_player_yaw     : float = 0.0;

// Helper to move a value towards a target by a fixed max delta
def move_towards(current : float; target : float; max_delta : float) : float
{
    if (abs(target - current) <= max_delta) { return target; }
    return current + (target > current ? max_delta : -max_delta);
}

[export]
def update_player_controller(world : World?; dt : float; player : uint; idleClip : int; walkClip : int; inputX : float; inputZ : float; camId : uint; is_sprinting : bool; runClip : int)
{
    if (player == 0u) { return; }

    // ── 1. Camera Relative Vectors (Flattened to XZ plane) ───────────────────
    // Orbit cameras keep their live rotation in orbit yaw; get_camera_forward()
    // reads the free-camera yaw, which stays at its startup value.
    var camFwd = float3(0.0, 0.0, -1.0);
    var camRight = float3(1.0, 0.0, 0.0);
    if (camId != 0u)
    {
        let yaw = get_camera_yaw(camId) * 0.01745329252;
        camFwd = float3(-sin(yaw), 0.0, -cos(yaw));
        camRight = float3(cos(yaw), 0.0, -sin(yaw));
    }
    
    let f_len = sqrt(camFwd.x * camFwd.x + camFwd.z * camFwd.z);
    let r_len = sqrt(camRight.x * camRight.x + camRight.z * camRight.z);
    
    // Normalize safely
    if (f_len > 0.001) { camFwd.x /= f_len; camFwd.z /= f_len; }
    if (r_len > 0.001) { camRight.x /= r_len; camRight.z /= r_len; }

    // ── 2. Target Velocity Calculation ───────────────────────────────────────
    let moveDirX = camFwd.x * inputZ + camRight.x * inputX;
    let moveDirZ = camFwd.z * inputZ + camRight.z * inputX;
    let moveLen = sqrt(moveDirX * moveDirX + moveDirZ * moveDirZ);

    var targetVelX = 0.0;
    var targetVelZ = 0.0;

    if (moveLen > 0.001)
    {
        // Normalize diagonal movement so it isn't faster
        let normX = moveDirX / moveLen;
        let normZ = moveDirZ / moveLen;
        let base_speed = is_sprinting ? g_sprint_speed : g_walk_speed;
        targetVelX = normX * base_speed;
        targetVelZ = normZ * base_speed;
    }

    // ── 3. Acceleration / Deceleration (Momentum) ────────────────────────────
    let accel = (moveLen > 0.001) ? g_acceleration : g_deceleration;
    g_player_vel_x = move_towards(g_player_vel_x, targetVelX, accel * dt);
    g_player_vel_z = move_towards(g_player_vel_z, targetVelZ, accel * dt);

    // ── 4. Apply Movement ────────────────────────────────────────────────────
    var pos = get_position(world, player);
    pos.x += g_player_vel_x * dt;
    pos.z += g_player_vel_z * dt;

    // ── 5. Smooth Rotation (Shortest Path Yaw Lerp) ──────────────────────────
    let actual_speed = sqrt(g_player_vel_x * g_player_vel_x + g_player_vel_z * g_player_vel_z);
    
    // Only rotate if we are actually moving
    if (actual_speed > 0.1)
    {
        let targetYaw = atan2(g_player_vel_x, g_player_vel_z) * 57.2958;
        
        // Find the shortest angular distance to turn
        var delta = targetYaw - g_player_yaw;
        while (delta > 180.0) { delta -= 360.0; }
        while (delta < -180.0) { delta += 360.0; }
        
        // Lerp the yaw smoothly
        let turn_amount = min(1.0, g_rotation_speed * dt);
        g_player_yaw += delta * turn_amount;
    }

    // Apply final transform
    let euler = float3(0.0, g_player_yaw, 0.0);
    let scale = get_scale(world, player);
    set_transform(world, player, pos, euler, scale);

    // ── 6. Animation State Management ────────────────────────────────────────
    let is_moving = actual_speed > 0.2;
    let target_clip = is_moving ? (is_sprinting && runClip >= 0 ? runClip : walkClip) : idleClip;
    
    if (target_clip >= 0 && target_clip != g_current_clip)
    {
        set_animation(world, player, target_clip);
        // set_animation_blend(world, player, target_clip, g_anim_blend_spd); // Broken atm
        g_current_clip = target_clip;
    }

    // ── 7. Dynamic Playback Speed (Syncs animation to actual speed) ──────────
    // This prevents "ice-skating" where the feet slide on the floor.
    if (is_moving)
    {
        let base_speed = is_sprinting ? g_sprint_speed : g_walk_speed;
        let speed_ratio = actual_speed / base_speed;
        
        // Map ratio (0 to 1) to anim speed (0.6 to 1.2)
        let anim_speed = 0.6 + speed_ratio * 0.6;
        
        if (is_sprinting && runClip >= 0) {
            set_playback_speed(world, player, clamp(anim_speed * 1.1, 0.8, 1.4));
        } else {
            set_playback_speed(world, player, clamp(anim_speed, 0.6, 1.2));
        }
    }
    else
    {
        // Breathing idle speed
        set_playback_speed(world, player, 1.0);
    }
}
