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
///   - Height follows the ground, not Crash: a plain jump (2.2 up) leaves eye and look point where
///     they were. Once his feet climb past the look point (double jump, bounce crate, worm launch)
///     the look point rides with them and the eye follows 0.17 s behind. Falling, both hold until he
///     drops 0.5 below the raised ground, then track 0.5 above his feet; the look point settles
///     within ~0.05 s, the eye still 0.17 s behind (rig bounce_rig_*.csv, logs/cameralean).
///   - The look point leads to the side Crash faces: 0.86 x the sine of his facing off the view at
///     rest (0.84 after a run across, 0.68 after a tap turn), ~1.4 running across, easing over ~0.6 s,
///     so he sits off-centre toward the side he is not facing. Turned toward the camera it also
///     slides toward the eye, 1.93 x the facing's toward-camera share (9.17 away facing straight at it).
///     (rig lean_rig.csv, Crash's world matrix at 0x00CF0A70, logs/cameralean).
///   - The right stick orbits it round him at ~115 deg/s at full deflection.
///   - Scenery between Crash and the eye pulls the eye in along the line; it eases back out (0.2 s).
///     Against Hub B's cliff by worm 18 (rig slot 13, logs/camcollapse) an orbit into the rock pulls the eye
///     to ~1.9 flat, still on the line and still at the same pitch, and it holds there: it neither rises
///     nor swings, and Crash drops out of the bottom of the frame.
/// The beach's camera section holds 13 trigger boxes, all with both camera types None (3): no
/// authored cameras replace this one anywhere on the beach.
/// </summary>
public sealed class CrashCamera
{
	public const float FovDegrees = 45.0f;
	private const float FollowLag = 0.17f;
	private const float LeadFacing = 0.86f;
	private const float LeadVelocity = 0.07f;
	private const float LeadPush = 1.93f;
	private const float LeadLag = 0.6f;
	// Collision: a 0.3 sphere cast (the near plane must stay outside terrain) pulls the eye in at once and
	// eases back out over about 1.3 s - the rig holds a pull for a good second before it settles (rig
	// orb_cliff/orb_pillar in rig_samples.csv: in within a frame, out over ~1.4 s).
	private const float PullOutLag = 0.45f;
	private const float WallMargin = 0.05f;
	private const float CameraSkin = 0.3f;
	public const float StickTurnRate = 115.0f;
	// Airborne, the height reference holds between his feet less the look height (a launch past the look
	// point lifts it) and his feet plus DropFollow (a fall drags it down). The look point's height eases
	// to it faster than the eye (rig djump/worm1: ~1 frame at 50 Hz up, ~0.05 s down and on landing).
	private const float DropFollow = 0.5f;
	private const float AimLag = 0.05f;

	// Pitch (rig, logs/camera pitch*): the right stick's Y swings the camera up over Crash (stick up) or down
	// behind him (stick down) at 59 deg/s at full deflection, between 75 deg above and 35 deg below. The rate
	// eases in (0.16 s) and fades over the last 12.5 deg before each limit. The camera does not recentre by
	// itself while Crash stands still. Once he moves, it returns to 15 deg at 46 deg/s.
	private const float DefaultPitch = 15.0f;
	private const float MinPitch = -35.0f;
	private const float MaxPitch = 75.0f;
	public const float StickPitchRate = 59.0f;
	public const float StickEase = 0.16f;
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

	public Entity Entity { get; private set; }

	private Vector3 _eye;
	private float _groundY;
	private float _aimGroundY;
	private float _heldGroundY;
	private float _lead;
	private float _push;
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

	/// <summary>After a scene reload destroyed the camera entity (Playground R reset): a fresh orbit camera
	/// at this rig's own eye and aim, every easing state kept, so the view does not move.</summary>
	public void Rebind()
	{
		Entity camera = Camera.CreateOrbit(_eye, _aim, FovDegrees);
		camera.Name = "Crash Camera";
		Camera.SetMain(camera);
		Entity = camera;
	}

	/// <summary>Straight behind <paramref name="feet"/> for a facing (spawn, respawn), no easing.</summary>
	public void Snap(Vector3 feet, float facingDegrees)
	{
		float r = facingDegrees * (MathF.PI / 180.0f);
		_groundY = _aimGroundY = _heldGroundY = feet.Y;
		_lead = _push = 0.0f;
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
		_heldGroundY = grounded ? feet.Y : Math.Clamp(_heldGroundY, feet.Y - Table(s_aim, _pitch), feet.Y + DropFollow);
		float follow = 1.0f - MathF.Exp(-dt / FollowLag);
		_groundY += (_heldGroundY - _groundY) * follow;
		_aimGroundY += (_heldGroundY - _aimGroundY) * (1.0f - MathF.Exp(-dt / AimLag));

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
		Vector2 face = new(facing.X, facing.Z);
		float lead = Vector2.Dot(face, right) * LeadFacing + Vector2.Dot(new Vector2(velocity.X, velocity.Z), right) * LeadVelocity;
		float push = MathF.Max(0.0f, -Vector2.Dot(face, view)) * LeadPush;
		float leadEase = 1.0f - MathF.Exp(-dt / LeadLag);
		_lead += (lead - _lead) * leadEase;
		_push += (push - _push) * leadEase;
		Vector2 focus = new Vector2(feet.X, feet.Z) + right * _lead - view * _push;

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

		// The look point never lags below his feet: on a launch it rides with him while the eye catches up.
		// The lead and the toward-camera slide must not carry it into scenery: it stops short of whatever lies
		// between it and the point over his head (facing the camera against a cliff pushed it into the rock,
		// every cast then started inside and the eye collapsed onto it; HubRun5 worm 18).
		Vector3 aim = new(focus.X, MathF.Max(_aimGroundY + aimHeight, feet.Y), focus.Y);
		aim = Clear(new Vector3(feet.X, aim.Y, feet.Z), aim);
		Vector3 ray = _eye - aim;
		float length = ray.Length();
		if (length < 1e-3f)
		{
			Apply(aim, _eye);
			return;
		}
		// However far scenery pulls the eye in, it looks along the string's line: a pull right onto the look
		// point must not leave the view (and the stick basis read from it) without a direction.
		Vector3 seen = Collide(aim, ray, length, dt);
		Apply(seen - ray, seen);
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
	// eases back out when the way clears. Only level collision counts, not crates or wildlife. The
	// cast carries a skin so the near plane never enters terrain even on a grazing pass.
	private Vector3 Collide(Vector3 aim, Vector3 ray, float length, float dt)
	{
		float allowed = Blocked(aim, ray / length, length) / length;
		_pull = allowed < _pull ? allowed : _pull + (allowed - _pull) * (1.0f - MathF.Exp(-dt / PullOutLag));
		return aim + ray * _pull;
	}

	// The look point, stopped short of level collision between the point over his head and it.
	private static Vector3 Clear(Vector3 from, Vector3 to)
	{
		Vector3 ray = to - from;
		float length = ray.Length();
		return length < 1e-3f ? to : from + ray * (Blocked(from, ray / length, length) / length);
	}

	// How far a skinned sphere gets along a ray before level collision, less the wall margin. The cast reports
	// only its closest body, so a crate, creature or pickup in the way is stepped past and the rest recast:
	// otherwise it would hide the rock behind it.
	private static float Blocked(Vector3 from, Vector3 direction, float length)
	{
		float along = 0.0f;
		for (int i = 0; i < 4 && along < length; i++)
		{
			RaycastHit hit = Physics.SphereCast(from + direction * along, direction, CameraSkin, length - along);
			if (!hit.DidHit)
			{
				return length;
			}
			float at = along + hit.Fraction * (length - along);
			if (hit.Entity.Name == "Collision")
			{
				return Math.Max(0.0f, at - WallMargin);
			}
			along = at + 2.0f * CameraSkin;
		}
		return length;
	}

	private void Apply(Vector3 aim, Vector3 eye)
	{
		_aim = aim;
		Camera.SetTarget(Entity, aim);
		Camera.SetPosition(Entity, eye);
	}
}
