using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Crash's follow camera, as the original PAL game runs it on N. Sanity Beach.
///
/// Measured on the CrashModded PCSX2 rig by reading the game's own camera from EE RAM (world matrix
/// at 0x00CFEE00, projection at 0x0098EB20) every frame while Crash stood, ran, turned and jumped
/// from the beach spawn (logs/camera: rig_samples.csv, rigcam.py, analyze.py). What the rig showed:
///   - Vertical FOV 45 deg (projection [1][1] = 2.4149, 4:3).
///   - At rest the eye sits 7.24 back and 4.44 above the ground under Crash, looking at a point
///     2.5 above it: 15 deg down, 7.5 from the look point.
///   - The eye is on a string: it keeps its bearing and closes the horizontal distance back to 7.24
///     with a 0.17 s lag. Running away pulls it out to ~8.9, running at it pushes it in, and
///     running sideways swings it round behind him (~75 deg/s at run speed). Standing still, it
///     never swings back behind him on its own.
///   - Height follows the ground, not Crash: a jump leaves eye and look point where they were.
///   - The look point leads to the side Crash faces (0.55 at rest, ~1.3 running across the view),
///     easing over ~0.5 s, so he sits off-centre toward the side he is not facing.
///   - The right stick orbits it round him at ~115 deg/s at full deflection.
///   - Scenery between Crash and the eye pulls the eye in along the line; it eases back out (0.2 s).
/// The beach's camera section holds 13 trigger boxes, all with both camera types None (3): no
/// authored cameras replace this one anywhere on the beach.
/// </summary>
public sealed class CrashCamera
{
	public const float FovDegrees = 45.0f;
	private const float Back = 7.24f;
	private const float EyeHeight = 4.44f;
	private const float AimHeight = 2.5f;
	private const float FollowLag = 0.17f;
	private const float LeadFacing = 0.55f;
	private const float LeadVelocity = 0.08f;
	private const float LeadLag = 0.5f;
	private const float PullOutLag = 0.2f;
	private const float WallMargin = 0.3f;
	private const float StickTurnRate = 115.0f;
	// Airborne, the height reference holds; it only follows him down once he falls this far below it.
	private const float DropFollow = 1.0f;

	public Entity Entity { get; }

	private Vector3 _eye;
	private float _groundY;
	private float _heldGroundY;
	private float _lead;
	private float _pull = 1.0f;

	public CrashCamera(Vector3 feet, float facingDegrees)
	{
		Entity camera = Camera.CreateOrbit(feet + new Vector3(0.0f, EyeHeight, Back), feet, FovDegrees);
		camera.Name = "Crash Camera";
		Camera.SetMain(camera);
		Entity = camera;
		Snap(feet, facingDegrees);
	}

	/// <summary>Straight behind <paramref name="feet"/> for a facing (spawn, respawn), no easing.</summary>
	public void Snap(Vector3 feet, float facingDegrees)
	{
		float r = facingDegrees * (MathF.PI / 180.0f);
		_groundY = _heldGroundY = feet.Y;
		_lead = 0.0f;
		_pull = 1.0f;
		_eye = new Vector3(feet.X + MathF.Sin(r) * Back, feet.Y + EyeHeight, feet.Z + MathF.Cos(r) * Back);
		Apply(new Vector3(feet.X, feet.Y + AimHeight, feet.Z), _eye);
	}

	/// <summary>One frame. <paramref name="turnDegrees"/> is this frame's manual orbit (mouse);
	/// <paramref name="stick"/> the right stick's x; <paramref name="lift"/> raises the whole view
	/// (a drowning body floating up).</summary>
	public void Update(float dt, Vector3 feet, bool grounded, Vector3 facing, Vector3 velocity, float turnDegrees, float stick, float lift)
	{
		if (grounded || feet.Y < _heldGroundY - DropFollow)
		{
			_heldGroundY = feet.Y;
		}
		float follow = 1.0f - MathF.Exp(-dt / FollowLag);
		_groundY += (_heldGroundY - _groundY) * follow;

		// Bearing from the eye to him, flat; the lead is across it.
		Vector2 toCrash = new(feet.X - _eye.X, feet.Z - _eye.Z);
		Vector2 view = toCrash.LengthSquared() > 1e-6f ? Vector2.Normalize(toCrash) : new Vector2(0.0f, -1.0f);
		Vector2 right = new(-view.Y, view.X);
		float lead = Vector2.Dot(new Vector2(facing.X, facing.Z), right) * LeadFacing
			+ Vector2.Dot(new Vector2(velocity.X, velocity.Z), right) * LeadVelocity;
		_lead += (lead - _lead) * (1.0f - MathF.Exp(-dt / LeadLag));
		Vector2 focus = new Vector2(feet.X, feet.Z) + right * _lead;

		// Manual orbit about the focus, then the string pulls the eye back to Back.
		Vector2 offset = new(_eye.X - focus.X, _eye.Z - focus.Y);
		float turn = (turnDegrees + stick * StickTurnRate * dt) * (MathF.PI / 180.0f);
		if (turn != 0.0f)
		{
			float c = MathF.Cos(turn), s = MathF.Sin(turn);
			offset = new Vector2(offset.X * c + offset.Y * s, -offset.X * s + offset.Y * c);
		}
		float d = offset.Length();
		Vector2 bearing = d > 1e-4f ? offset / d : -view;
		Vector2 eye = focus + bearing * (d + (Back - d) * follow);
		_eye = new Vector3(eye.X, _groundY + EyeHeight, eye.Y);

		Vector3 aim = new(focus.X, _groundY + AimHeight + lift, focus.Y);
		Vector3 wanted = _eye + new Vector3(0.0f, lift, 0.0f);
		Apply(aim, Collide(aim, wanted, dt));
	}

	// Scenery between the look point and the eye pulls the eye in to just short of it at once; it
	// eases back out when the way clears. Only level collision counts, not crates or wildlife.
	private Vector3 Collide(Vector3 aim, Vector3 eye, float dt)
	{
		Vector3 ray = eye - aim;
		float length = ray.Length();
		float allowed = 1.0f;
		RaycastHit hit = Physics.Raycast(aim, ray / length, length);
		if (hit.DidHit && hit.Entity.Name == "Collision")
		{
			allowed = Math.Max(0.0f, hit.Fraction * length - WallMargin) / length;
		}
		_pull = allowed < _pull ? allowed : _pull + (allowed - _pull) * (1.0f - MathF.Exp(-dt / PullOutLag));
		return aim + ray * _pull;
	}

	private void Apply(Vector3 aim, Vector3 eye)
	{
		Camera.SetTarget(Entity, aim);
		Camera.SetPosition(Entity, eye);
	}
}
