options gen2;

require world;
require animation;
require camera;
require math;

var g_move_speed     : float = 3.0;
var g_anim_blend_spd : float = 4.0;
var g_current_clip   : int = -1;

[export]
def update_player_controller(world : World?; dt : float; player : uint; idleClip : int; walkClip : int; inputX : float; inputZ : float; camId : uint)
{
	if (player == 0u) { return; }

	// Camera-relative movement: W moves in the direction the camera is facing.
	var camFwd = float3(0.0, 0.0, -1.0);
	var camRight = float3(1.0, 0.0, 0.0);
	if (camId != 0u)
	{
		camFwd = get_camera_forward(camId);
		camRight = get_camera_right(camId);
	}

	// Build movement in camera space then transform to world.
	// inputZ: +1 = W (forward), -1 = S (backward)
	// inputX: -1 = A (left),   +1 = D (right)
	let moveDir = camFwd * inputZ + camRight * inputX;
	let len = sqrt(moveDir.x * moveDir.x + moveDir.z * moveDir.z);

	let is_moving = len > 0.001;
	var pos = get_position(world, player);
	var euler = get_euler(world, player);
	let scale = get_scale(world, player);

	if (is_moving)
	{
		let nx = moveDir.x / len;
		let nz = moveDir.z / len;

		pos.x += nx * g_move_speed * dt;
		pos.z += nz * g_move_speed * dt;

		// Face the movement direction (yaw only).
		let targetYaw = atan2(nx, nz) * 57.2958;
		euler = float3(0.0, targetYaw, 0.0);
	}
	else
	{
		euler = float3(0.0, euler.y, 0.0);
	}

	// set_transform propagates to spawned model entities; set_position alone only
	// updates the script parent transform.
	set_transform(world, player, pos, euler, scale);

	// ── Animation state ────────────────────────────────────
	let target_clip = is_moving ? walkClip : idleClip;
	if (target_clip >= 0 && target_clip != g_current_clip)
	{
		set_animation_blend(world, player, target_clip, g_anim_blend_spd);
		set_playback_speed(world, player, is_moving ? 0.8 : 1.0);
		g_current_clip = target_clip;
	}
}
