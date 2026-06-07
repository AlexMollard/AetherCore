options gen2;

require world;
require animation;
require physics;
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
	if (is_moving)
	{
		let nx = moveDir.x / len;
		let nz = moveDir.z / len;

		// Apply velocity to physics body.
		set_linear_velocity(world, player, nx * g_move_speed, 0.0, nz * g_move_speed);

		// Face the movement direction (yaw only).
		let targetYaw = atan2(nx, nz) * 57.2958;
		let euler = get_euler(world, player);
		set_euler(world, player, float3(euler.x, targetYaw, euler.z));
	}
	else
	{
		// Stop horizontal movement when no input.
		set_linear_velocity(world, player, 0.0, 0.0, 0.0);
	}

	// Sync visual position from physics position so renderer follows physics.
	let physPos = get_physics_position(world, player);
	if (physPos.x != 0.0 || physPos.y != 0.0 || physPos.z != 0.0)
	{
		set_position(world, player, physPos);
	}

	// ── Animation state ────────────────────────────────────
	let target_clip = is_moving ? walkClip : idleClip;
	if (target_clip >= 0 && target_clip != g_current_clip)
	{
		set_animation_blend(world, player, target_clip, g_anim_blend_spd);
		set_playback_speed(world, player, is_moving ? 0.8 : 1.0);
		g_current_clip = target_clip;
	}
}