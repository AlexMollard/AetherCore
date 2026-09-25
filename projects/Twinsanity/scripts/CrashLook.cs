using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Crash's upper body turns toward where the right stick pans the camera, as in the original PAL game.
///
/// Measured on the CrashModded PCSX2 rig from Crash's model-space pose in EE RAM (joints j11-j14 from
/// 0x00CF2FF0, stride 0xA0) while the camera was panned (logs/camera: rig_head.npy, headan.py,
/// headlook_rig.png). What the rig showed:
///   - Four joints turn together, not the head alone: spine (j11), chest (j12), neck (j13) and head (j14).
///   - The pose follows the stick's direction, not the camera's angle. It holds for as long as the stick
///     is held, even through a full orbit, and it is the same at 0.6 and full deflection. It blends in
///     over about 0.25 s and goes back to the animation about 0.35 s after the stick is released.
///   - Panning sideways yaws the four joints 23/16/20/11 degrees toward the pan, with 4-11 degrees of roll.
///     Stick down (camera low, looking up) pitches them 24/18/23/15 degrees up; stick up (camera high)
///     pitches them 12/8/10/6 degrees down.
/// The offsets below are the rig's local rotation vectors (degrees) with the extract's X mirror applied:
/// x keeps its sign, y and z flip.
/// </summary>
public sealed class CrashLook
{
	private static readonly string[] s_joints = { "joint11", "joint12", "joint13", "joint14" };
	// Stick right; stick left is the negation.
	private static readonly Vector3[] s_side = { new(0.0f, 23.0f, -4.0f), new(0.0f, 16.0f, -6.5f), new(-1.0f, 20.0f, -11.0f), new(-1.0f, 11.0f, -10.0f) };
	// Stick down: camera low, Crash looks up.
	private static readonly Vector3[] s_up = { new(-24.0f, 0.0f, 0.0f), new(-18.0f, 0.0f, 0.0f), new(-23.0f, 0.0f, 0.0f), new(-15.0f, 0.0f, 0.0f) };
	// Stick up: camera high, Crash looks down.
	private static readonly Vector3[] s_down = { new(12.0f, 0.0f, 0.0f), new(8.0f, 0.0f, 0.0f), new(10.0f, 0.0f, 0.0f), new(6.0f, 0.0f, 0.0f) };
	private const float BlendIn = 0.25f;
	private const float Release = 0.35f;
	private const float StickThreshold = 0.1f;

	private float _side;
	private float _vertical;
	private bool _applied;

	/// <summary>One frame: <paramref name="stick"/> is the right stick (Y up), zero when Crash cannot look.</summary>
	public void Update(Entity model, Vector2 stick, float dt)
	{
		_side = Step(_side, Target(stick.X), dt);
		_vertical = Step(_vertical, Target(stick.Y), dt);
		if (_side == 0.0f && _vertical == 0.0f)
		{
			if (_applied)
			{
				foreach (string joint in s_joints)
				{
					Animation.SetJointOffset(model, joint, Quaternion.Identity);
				}
				_applied = false;
			}
			return;
		}
		for (int i = 0; i < s_joints.Length; i++)
		{
			Vector3 degrees = s_side[i] * _side + (_vertical > 0.0f ? s_down[i] * _vertical : s_up[i] * -_vertical);
			Animation.SetJointOffset(model, s_joints[i], FromRotationVector(degrees));
		}
		_applied = true;
	}

	private static float Target(float axis) => MathF.Abs(axis) > StickThreshold ? MathF.Sign(axis) : 0.0f;

	// Toward a held direction at 1/BlendIn per second; back to rest (and through it, on a reversal) at 1/Release.
	private static float Step(float weight, float target, float dt)
	{
		bool easingIn = target != 0.0f && MathF.Sign(target) == MathF.Sign(weight) || weight == 0.0f;
		float rate = (easingIn ? 1.0f / BlendIn : 1.0f / Release) * dt;
		float goal = easingIn ? target : 0.0f;
		return weight + Math.Clamp(goal - weight, -rate, rate);
	}

	private static Quaternion FromRotationVector(Vector3 degrees)
	{
		float angle = degrees.Length() * (MathF.PI / 180.0f);
		return angle < 1e-6f ? Quaternion.Identity : Quaternion.CreateFromAxisAngle(Vector3.Normalize(degrees), angle);
	}
}
