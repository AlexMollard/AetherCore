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
	private const float FollowLag = 0.17f;
	private const float LeadFacing = 0.55f;
	private const float LeadVelocity = 0.08f;
	private const float LeadLag = 0.5f;
	private const float PullOutLag = 0.2f;
	private const float WallMargin = 0.3f;
	private const float StickTurnRate = 115.0f;
	// Airborne, the height reference holds; it only follows him down once he falls this far below it.
	private const float DropFollow = 1.0f;

	// Pitch (rig, logs/camera pitch*): the right stick's Y swings the camera up over Crash (stick up) or down
	// behind him (stick down) at 59 deg/s at full deflection, between 75 deg above and 35 deg below. The rate
	// eases in (0.16 s) and fades over the last 12.5 deg before each limit. The camera does not recentre by
	// itself while Crash stands still. Once he moves, it returns to 15 deg at 46 deg/s.
	private const float DefaultPitch = 15.0f;
	private const float MinPitch = -35.0f;
	private const float MaxPitch = 75.0f;
	private const float StickPitchRate = 59.0f;
	private const float StickEase = 0.16f;
	private const float LimitEase = 12.5f;
	private const float RecentreRate = 46.0f;
	private const float RecentreSpeed = 1.0f;
	// Distance from the look point and the look point's height above the ground, as a function of pitch.
	// The rig's camera is not a fixed-radius orbit: it moves further out as it rises.
	private static readonly float[] s_pitchKeys = { -35.0f, 0.0f, 15.0f, 45.0f, 75.0f };
	private static readonly float[] s_radius = { 2.88f, 6.04f, 7.47f, 10.13f, 12.87f };
	private static readonly float[] s_aim = { 1.65f, 2.14f, 2.5f, 3.04f, 3.5f };

	// Drowning (rig c_drownN in rig_samples.csv, logs/camera/drown_*.png): the follow stops and a fixed move
	// takes over, sampled every 0.1 s from the moment he goes under the drowning plane, against his body and
	// the water surface. The eye keeps its follow pose for 0.2 s, rises over him to look almost straight down
	// (87 deg, 11.4 above the water, at 0.9 s), then swings back out along the same bearing and settles 9.3
	// back and 3.3 above the water, looking 26 deg down at him (2.8 s), where it holds until the respawn.
	private const float DrownStep = 0.1f;
	private const float DrownBlend = 0.3f;
	private static readonly float[] s_drownBack = { 7.13f, 7.21f, 7.28f, 7.19f, 6.89f, 6.40f, 5.70f, 4.40f, 2.84f, 1.98f, 2.40f, 3.14f, 4.05f, 5.07f, 6.15f, 7.16f, 7.92f, 8.49f, 8.89f, 9.17f, 9.31f, 9.34f, 9.31f, 9.29f, 9.28f, 9.28f, 9.27f, 9.27f, 9.27f, 9.27f, 9.27f };
	private static readonly float[] s_drownHeight = { 4.90f, 4.82f, 4.72f, 5.19f, 6.17f, 7.32f, 8.52f, 9.84f, 10.90f, 11.41f, 11.66f, 11.84f, 11.93f, 11.91f, 11.78f, 11.41f, 10.66f, 9.74f, 8.69f, 7.61f, 6.51f, 5.43f, 4.48f, 3.94f, 3.66f, 3.51f, 3.42f, 3.38f, 3.36f, 3.35f, 3.34f };
	private static readonly float[] s_drownPitch = { 26.6f, 27.2f, 27.7f, 31.7f, 39.0f, 47.7f, 57.0f, 68.9f, 80.6f, 86.9f, 86.8f, 83.5f, 79.3f, 74.8f, 70.1f, 65.4f, 60.8f, 56.1f, 51.3f, 46.6f, 41.8f, 36.9f, 32.5f, 29.7f, 28.1f, 27.3f, 26.8f, 26.6f, 26.4f, 26.4f, 26.3f };

	public Entity Entity { get; }

	private Vector3 _eye;
	private float _groundY;
	private float _heldGroundY;
	private float _lead;
	private float _pull = 1.0f;
	private float _pitch = DefaultPitch;
	private float _pitchRate;
	private float _yawRate;
	private float _flat = Flat(DefaultPitch);
	private Vector3 _aim;
	private float _drownClock = -1.0f;
	private Vector2 _drownBearing;
	private Vector3 _drownFrom;

	public CrashCamera(Vector3 feet, float facingDegrees)
	{
		Entity camera = Camera.CreateOrbit(feet + new Vector3(0.0f, 4.44f, 7.24f), feet, FovDegrees);
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
		_pitch = DefaultPitch;
		_pitchRate = _yawRate = 0.0f;
		_flat = Flat(_pitch);
		float rise = Table(s_radius, _pitch) * MathF.Sin(_pitch * (MathF.PI / 180.0f));
		_eye = new Vector3(feet.X + MathF.Sin(r) * _flat, feet.Y + Table(s_aim, _pitch) + rise, feet.Z + MathF.Cos(r) * _flat);
		Apply(new Vector3(feet.X, feet.Y + Table(s_aim, _pitch), feet.Z), _eye);
	}

	/// <summary>One frame. <paramref name="turnDegrees"/> and <paramref name="pitchDegrees"/> are this
	/// frame's manual orbit and pitch (mouse); <paramref name="stick"/> is the right stick (Y up).</summary>
	public void Update(float dt, Vector3 feet, bool grounded, Vector3 facing, Vector3 velocity, float turnDegrees, float pitchDegrees, Vector2 stick)
	{
		_drownClock = -1.0f;
		if (grounded || feet.Y < _heldGroundY - DropFollow)
		{
			_heldGroundY = feet.Y;
		}
		float follow = 1.0f - MathF.Exp(-dt / FollowLag);
		_groundY += (_heldGroundY - _groundY) * follow;

		// Stick rates ease in; pitch fades into its limits, and returns to the default while he moves.
		float ease = 1.0f - MathF.Exp(-dt / StickEase);
		_yawRate += (stick.X * StickTurnRate - _yawRate) * ease;
		_pitchRate += (stick.Y * StickPitchRate - _pitchRate) * ease;
		float room = _pitchRate > 0.0f ? MaxPitch - _pitch : _pitch - MinPitch;
		_pitch += _pitchRate * dt * Math.Clamp(room / LimitEase, 0.0f, 1.0f) + pitchDegrees;
		if (MathF.Abs(stick.Y) < 1e-3f && new Vector2(velocity.X, velocity.Z).Length() > RecentreSpeed)
		{
			float back = RecentreRate * dt;
			_pitch += Math.Clamp(DefaultPitch - _pitch, -back, back);
		}
		_pitch = Math.Clamp(_pitch, MinPitch, MaxPitch);

		// Bearing from the eye to him, flat; the lead is across it.
		Vector2 toCrash = new(feet.X - _eye.X, feet.Z - _eye.Z);
		Vector2 view = toCrash.LengthSquared() > 1e-6f ? Vector2.Normalize(toCrash) : new Vector2(0.0f, -1.0f);
		Vector2 right = new(-view.Y, view.X);
		float lead = Vector2.Dot(new Vector2(facing.X, facing.Z), right) * LeadFacing
			+ Vector2.Dot(new Vector2(velocity.X, velocity.Z), right) * LeadVelocity;
		_lead += (lead - _lead) * (1.0f - MathF.Exp(-dt / LeadLag));
		Vector2 focus = new Vector2(feet.X, feet.Z) + right * _lead;

		// Manual orbit about the focus. A pitch change rescales the flat distance at once; the string
		// then pulls the eye toward the flat distance for this pitch.
		Vector2 offset = new(_eye.X - focus.X, _eye.Z - focus.Y);
		float turn = (turnDegrees + _yawRate * dt) * (MathF.PI / 180.0f);
		if (turn != 0.0f)
		{
			float c = MathF.Cos(turn), s = MathF.Sin(turn);
			offset = new Vector2(offset.X * c + offset.Y * s, -offset.X * s + offset.Y * c);
		}
		float flat = Flat(_pitch);
		float d = offset.Length() * flat / _flat;
		_flat = flat;
		Vector2 bearing = d > 1e-4f ? offset / offset.Length() : -view;
		Vector2 eye = focus + bearing * (d + (flat - d) * follow);
		float aimHeight = Table(s_aim, _pitch);
		float rise = Table(s_radius, _pitch) * MathF.Sin(_pitch * (MathF.PI / 180.0f));
		_eye = new Vector3(eye.X, _groundY + aimHeight + rise, eye.Y);

		Vector3 aim = new(focus.X, _groundY + aimHeight, focus.Y);
		Apply(aim, Collide(aim, _eye, dt));
	}

	/// <summary>One frame of the drowning shot: <paramref name="body"/> is where he went under,
	/// <paramref name="surfaceY"/> the water surface above him.</summary>
	public void Drown(float dt, Vector3 body, float surfaceY)
	{
		if (_drownClock < 0.0f)
		{
			Vector2 away = new(_eye.X - body.X, _eye.Z - body.Z);
			_drownBearing = away.LengthSquared() > 1e-6f ? Vector2.Normalize(away) : new Vector2(0.0f, 1.0f);
			Vector3 look = Vector3.Normalize(_aim - _eye);
			_drownFrom = new Vector3(away.Length(), _eye.Y - surfaceY, MathF.Asin(-look.Y) * (180.0f / MathF.PI));
			_drownClock = 0.0f;
		}
		else
		{
			_drownClock += dt;
		}
		float k = _drownClock / DrownStep;
		int i = Math.Min((int)k, s_drownBack.Length - 2);
		float f = Math.Min(k - i, 1.0f);
		Vector3 row = Vector3.Lerp(new Vector3(s_drownBack[i], s_drownHeight[i], s_drownPitch[i]), new Vector3(s_drownBack[i + 1], s_drownHeight[i + 1], s_drownPitch[i + 1]), f);
		// Our follow pose at the moment of death eases into the rig's over the first 0.3 s.
		row += (_drownFrom - new Vector3(s_drownBack[0], s_drownHeight[0], s_drownPitch[0])) * Math.Max(0.0f, 1.0f - _drownClock / DrownBlend);
		float p = row.Z * (MathF.PI / 180.0f);
		_eye = new Vector3(body.X + _drownBearing.X * row.X, surfaceY + row.Y, body.Z + _drownBearing.Y * row.X);
		Apply(_eye + new Vector3(-_drownBearing.X * MathF.Cos(p), -MathF.Sin(p), -_drownBearing.Y * MathF.Cos(p)) * 10.0f, _eye);
	}

	private static float Flat(float pitch) => Table(s_radius, pitch) * MathF.Cos(pitch * (MathF.PI / 180.0f));

	private static float Table(float[] values, float pitch)
	{
		int i = 1;
		while (i < s_pitchKeys.Length - 1 && pitch > s_pitchKeys[i])
		{
			i++;
		}
		float t = Math.Clamp((pitch - s_pitchKeys[i - 1]) / (s_pitchKeys[i] - s_pitchKeys[i - 1]), 0.0f, 1.0f);
		return values[i - 1] + (values[i] - values[i - 1]) * t;
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
		_aim = aim;
		Camera.SetTarget(Entity, aim);
		Camera.SetPosition(Entity, eye);
	}
}
