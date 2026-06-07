options gen2;

require world;
require camera;

[export]
def update_third_person_camera(world : World?; camId : uint; player : uint)
{
	if (camId == 0u || player == 0u) { return; }

	let pos = get_position(world, player);

	// Orbit camera tracks the player; LMB drag to orbit, scroll to zoom.
	set_camera_target(camId, pos.x, pos.y, pos.z);
}
