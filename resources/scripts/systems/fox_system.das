options gen2

require world
require math
require animation

// Spawn `count` foxes arranged in a circle of the given radius.
def spawn_foxes(world : World?; count : int; radius : float; fox_tag : uint)
{
	let step = 6.2832 / float(count);
	for (i in range(count))
	{
		let angle = float(i) * step;
		let pos   = float3(cos(angle) * radius, 0.0, sin(angle) * radius);
		let yaw   = angle * 57.2958 + 90.0;

		let fox = entity_create(world);
		add_transform(world, fox);
		set_transform(world, fox, pos, float3(0.0, yaw, 0.0), float3(0.05, 0.05, 0.05));
		load_model(world, fox, "assets://models/Fox/Fox.mesh");

		// Play the Run animation, desynchronised per fox.
		set_animation(world, fox, 2);
		set_playback_speed(world, fox, 0.7);
		set_anim_time(world, fox, float(i) * 0.3);

		// Tag em
		add_tag(world, fox, fox_tag);
	}
}
