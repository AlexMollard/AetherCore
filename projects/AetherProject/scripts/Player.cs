using System.Numerics;
using AetherCore;

namespace AetherGame;

// Already attached to the Player in this scene - press Play and move with WASD.
// Speed below shows up in the Inspector and saves with the scene. Change it, Play again.
// F5 rebuilds and hot-reloads this file while Play is still running.
// To use it elsewhere: select an entity, then Add Component > Script > Player.
public sealed class Player : EntityScript
{
	public float Speed = 5.0f;

	public override void OnUpdate(float deltaTime)
	{
		// A/D or left/right. GetAxisRaw returns -1, 0 or +1.
		float x = -Input.GetAxisRaw(Key.A, Key.D) + -Input.GetAxisRaw(Key.Left, Key.Right);
		float z = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);
		Vector3 move = new(x, 0.0f, z);
		
		Vector3 normalizedMove = move.LengthSquared() > 0.0f ? Vector3.Normalize(move) : Vector3.Zero;

		// Fix gravity by only moving in the XZ plane. The RigidBodyRef component is required for this to work.
		normalizedMove = new Vector3(normalizedMove.X, 0.0f, normalizedMove.Z);

		Self.Get<RigidBodyRef>().LinearVelocity = normalizedMove * Speed;
	}
}
