using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The earth worm's squash-launch (act_EARTH_WORM OnLand, COM_EARTH_WORM_SQUASHLAUNCH): when Crash
/// lands on a popped-up worm the worm plays its squash clip (slot 0x0C) and the script runs
/// ApplyVelocity(gravity 50, distance Y 10) on him - a launch that tops out 10 m above the take-off
/// under 50 m/s^2, which is Crash's own AirGravity. The worm actor (TwinsanityActors.UpdateWorm)
/// decides when Crash lands on it and calls TryLaunch.
/// </summary>
public static class MechanicsWorm
{
	private const float LaunchGravity = 50.0f;
	private const float LaunchHeight = 10.0f;
	// ApplyVelocity(gravity 50, distance Y 10): sqrt(2 g h) = 31.62. The game takes that frame's gravity
	// before it moves, so the rig reads 30.6 on the take-off frame (older rig_burst.json samples caught
	// 29.6-30 a frame later). Measured true rise off worm1 (Crash's world matrix, logs/cameralean/
	// bounce_rig_worm1.csv): 9.69 m, apex 0.62 s after take-off. At 30 the engine rose 8.97 in 0.55 s.
	private static readonly float LaunchVelocity = MathF.Sqrt(2.0f * LaunchGravity * LaunchHeight);
	// On the rig he lands on the popped worm's top, 1.94 m over its hole (h 2.078 on hubb worm 35 at
	// 0.141), rides the squash and leaves from there: the apex is the same over every worm (hubb 35
	// and 20: 11.2 and 11.1 over the hole, logs/hubroute/rig_launch35.csv, rig_bounce2). The engine
	// worm has no top to stand on, so he is caught falling anywhere from 0.3 m up to that top (the
	// catch used to stop 1.6 up: he fell 0.34 m further through the worm's strike height, and its
	// lunge killed him on the head - logs/hubrun/worm_catch.txt); launch him as if from the top so a
	// late catch (a low frame rate) does not cost height (hubb worm 20 lost 1 m).
	public const float WormTop = 1.94f;

	/// <summary>Launch Crash, feet at <paramref name="crashY"/>, off the worm whose hole is at
	/// <paramref name="wormPos"/>. Returns false when he is not falling onto it (rising through it, or
	/// already launched this contact).</summary>
	public static bool TryLaunch(CrashPlayer player, Vector3 wormPos, float crashY)
	{
		if (player.Velocity.Y > 0.0f)
		{
			return false;
		}
		float below = MathF.Max(0.0f, wormPos.Y + WormTop - crashY);
		player.Bounce(MathF.Sqrt(LaunchVelocity * LaunchVelocity + 2.0f * LaunchGravity * below));
		return true;
	}
}

/// <summary>
/// Pushables: the objects whose spawn scripts give them SetContactRigid (a rigid-body contact
/// response) - act_WUMPA_NUT, act_BEACH_BALL, act_MONKEY_ROCK, act_RIGID_CANNON, the hay bale, the
/// barrel and act_GLOBAL_BOMB. Crash walking
/// into one pushes it (the behaviour scripts' IsPushingObject condition, which switches his walk
/// and run to the push clips a044/a045 - CrashPlayer.Pushing).
///
/// Measured on the rig (logs/mechanics/rig_burst.json, the nut's live centre at 0x00D54010):
///   - Contact holds Crash's feet 1.0-1.1 m from the nut's centre; the nut's centre rests 0.5615
///     above the ground (its script's SetLogicalRadius 0.56), not at the instance height.
///   - He is held up while it gets going: the nut goes from rest to his speed in ~12 frames
///     (about 40 m/s^2), then runs at his full speed (9 run, 2.5 walk) - pushing never slows him.
///   - Pushing at an angle moves it along the contact normal only.
///   - Let go, it rolls on, its speed decaying exponentially at 0.87/s on flat sand.
/// Spin and slide (rig, logs/chickenpush/push_spin.csv / push_slide.csv, the nut at 60 frames/s):
///   - A spin in contact launches it along the contact normal 3 frames after the spin starts: 33.7
///     m/s, decaying at 0.67/s (35 m in 1.8 s), with a 0.1 m hop.
///   - A slide into it throws it up in an arc on contact: 0.28 m pop, then 11.5 m/s up under 27.7
///     m/s^2 (peak 2.7 m above rest, 0.87 s in the air) and 8.2 m/s along the contact normal,
///     decaying at 0.85/s - the free-roll rate - so it lands 5 m away.
/// Each is a kinematic collision body the script drives, so Crash's controller is blocked by it
/// exactly as by scenery.
/// </summary>
public sealed partial class TwinsanityActors
{
	private sealed class Pushable
	{
		public Entity Model;
		public Entity Body;
		public Vector3 Center;       // body centre (world)
		public Vector3 ModelOffset;  // model origin - body centre
		public float Radius;         // collision radius
		public float RestHeight;     // centre height above the ground
		public bool Rolls;
		public float PushAccel;
		public float Damping;        // 1/s, exponential speed decay while free
		public Vector3 Velocity;     // horizontal
		public Quaternion Roll = Quaternion.Identity;
		public Quaternion Base = Quaternion.Identity;
		public float FreeDamping;    // 1/s, current free decay (Damping, or the spin launch's)
		public float VelocityY;      // while airborne
		public bool Airborne;
		public float LaunchFloor;    // ground height it was launched from
		public bool Pivots;          // turns about its origin instead of sliding (the cannon)
		public List<Entity> Hulls = new(); // collision that turns with a pivoting object
		public float YawRate;        // deg/s, a pivoting object's current turn (coasts briefly when let go)
		public Entity Button;        // the cannon's red button (disc object 779), turns with it
		public Entity ButtonHull;    // its disc hull: Crash stands on the button's top
		public bool Settled;         // RestHeight known against real ground (collision may load late)
		public bool NoLaunch;        // the bomb: a spin primes it (TwinsanityProps) instead of launching it
		public bool Pushed;          // Crash has pushed it (the bomb's s5, IsPushingObject)
		public bool Held;            // Crash has hold of it (IsPushingObject): pulled to his hold point
		public Vector3 HoldDir;      // his last push direction while held (kept while he stands still)
	}

	private readonly List<Pushable> _pushables = new();

	private const float CrashRadius = 0.4f;   // Beach.scene.toml capsule
	private const float CrashHeight = 1.95f;
	// Push magnetism (rig, logs/beachfeel/: per-frame EE RAM traces of the nut and the hay bale, fitted
	// by sim.py). Walking into an object within GrabAngle of his heading takes hold of it; from then on
	// it is servoed to the point just ahead of him along his last push direction: velocity target = his
	// measured velocity + (hold point - centre) * CenteringRate, reached at the kind's PushAccel. Held, it
	// re-centres onto his line over ~0.5 s (grabbed 0.5-0.7 m off-line), trails ~20-30 deg outside
	// his curve at 45-90 deg/s, and when he stops dead it overshoots ~0.7 m and is pulled back to
	// touch him. It lets go when his heading swings faster than TurnReleaseRate (a sharp turn), when
	// he runs off more than ReleaseAngle from it (a reversal), or when it ends up more than
	// HoldRelease beyond touching distance.
	private const float CenteringRate = 2.2f;              // 1/s
	private static readonly float GrabCos = MathF.Cos(50.0f * MathF.PI / 180.0f);    // rig: 42 deg grabbed, 57 missed
	// ...and only with the object's centre within GrabOffset of his line: a glancing walk slides him
	// round a bale or barrel (rig_push_hay_off*.csv: 0.9 m off it is bumped 0.55 m, 1.2 m off it
	// never moves), where the padded contact alone took hold 1.2 m off at ~47 deg.
	private const float GrabOffset = 1.0f;                 // m
	private static readonly float ReleaseCos = MathF.Cos(57.0f * MathF.PI / 180.0f); // rig: held at 30, lost at 66
	private const float ReleaseSpeed = 1.5f;               // m/s: turning while stopping keeps hold
	private const float HoldRelease = 1.9f;                // m beyond touching (rig: pulled back from 2.45)
	// Pushed, the rig holds it about touching (nut 0.9-1.1, bale 1.3-1.4 from his feet). The hold
	// point sits HoldGap past touching his capsule: any closer and the object blocks him, slowing him
	// under his commanded speed, which the feed-forward then outruns. At a run his transform trails
	// his physics capsule by ~0.15 m, so the object rides ~1.2 m from his drawn feet on a 0.55
	// collider; the nut's collider is its measured 0.45 contact instead (see PushableKind).
	private const float HoldGap = 0.1f;
	// ponytail: the game's prop gravity is not in the extracted data; Crash's own AirGravity (50)
	// stands in, scaled by 5/7 for a rolling sphere. It only matters on slopes.
	private const float SlopeGravity = 50.0f * 5.0f / 7.0f;
	private const float RestSpeed = 0.05f;
	// Spin / slide launches (rig, see the summary). One gravity fits the nut's slide arc and its
	// spin hop; the ball and rock reuse the nut's numbers (ponytail: not measured).
	private const float LaunchGravity = 27.7f;
	private const float SpinLaunchSpeed = 33.7f;
	private const float SpinLaunchHop = 2.35f;   // sqrt(2 g 0.1)
	private const float SpinLaunchDamping = 0.67f;
	private const float SpinLaunchDelay = 0.05f; // 3 frames after the spin starts
	private const float SlideLaunchSpeed = 8.2f;
	private const float SlideLaunchUp = 11.5f;
	private const float SlideLaunchPop = 0.28f;
	private float _spinClock = -1.0f;            // time since the current spin started; -1 = none
	private bool _spinLaunched, _slideLaunched;
	// His velocity, smoothed: commanded over PushSmoothing for the hold servo's push line, measured
	// (feet displacement) over ReleaseSmoothing for the release test. On the rig a held object keeps
	// its line for ~2 frames after he turns (logs/beachfeel/rig_push_nut_turn90.csv: still straight
	// at 30 deg off, ~1.8 m/s sideways once let go), where the stick - and his velocity - turn at
	// once and a servo on them dragged it sideways at 4.4 m/s.
	private Vector3 _lastFeet, _smoothVel, _smoothCmd, _lastCmdDir;
	private bool _hasLastFeet;
	private const float PushSmoothing = 0.05f;             // s
	private const float ReleaseSmoothing = 0.02f;          // s
	// A sharp turn lets go at once: the rig holds through curves at 45-90 deg/s, but in a 90 deg
	// turn (his heading swinging at ~750 deg/s) the object leaves on its old line within ~2 frames.
	private const float TurnReleaseRate = 300.0f;          // deg/s of his commanded heading
	// The cannon (rig, logs/cannon/rigturn.json leg 1, and logs/traversal/cannon_push2.csv): its
	// origin never moves. Crash walking into a hull - a handle, the trail - drags the contact point
	// along with his tangential motion (the rig's yaw rate tracks his ω about the pivot within ~10%),
	// never faster than ~60 deg/s (the rig's fastest 0.1 s window, 62): running out through a handle
	// 5.5 m out it turns ~35 deg in 0.8 s and holds him to ~5 m/s. Let go, it coasts to a stop in
	// ~0.1 s (engine twin: logs/cannon/eng_turn.py, sheet_turn.png).
	private const float PivotMaxRate = 60.0f;  // deg/s
	private const float PivotCoastDecay = 14.0f; // 1/s

	// The button sits in the mount on the rear box of the trail plate, just short of the handles (rig
	// logs/cannon/rig_asm*.png, triangulated from four views: local (0.36, 0.47, -3.85) for its top's
	// centre; Crash standing on it reads 1.692 = pivot + 0.48, rigfire.json). The box is centred on
	// x = 0, z = -3.85 (hull 2), so the model's origin (base of a 0.46-tall disc) is 0.02 up.
	private static readonly Vector3 CannonButtonLocal = new(0.0f, 0.02f, -3.85f);
	private const string CannonButtonModel = "project://assets/models/objects/RIGID_CANNON_BUTTON/RIGID_CANNON_BUTTON.gltf";
	private const string CannonButtonHull = "project://assets/models/objects/RIGID_CANNON_BUTTON/RIGID_CANNON_BUTTON_hull0.gltf";

	// Engine yaw is atan2(x, z): local +z maps to (sin y, cos y).
	private static Vector3 Yawed(Vector3 local, float yawDeg)
	{
		float yaw = yawDeg * MathF.PI / 180.0f;
		float s = MathF.Sin(yaw), c = MathF.Cos(yaw);
		return new Vector3(local.X * c + local.Z * s, local.Y, -local.X * s + local.Z * c);
	}

	// Per family: collision radius (model extent - with our 0.4 capsule it reproduces the rig's
	// 1.0-1.1 m contact distance), centre height above ground (the script's SetLogicalRadius),
	// rolls, push acceleration (m/s^2, the hold servo's limit; rig fit 50), free-roll decay (1/s).
	// The nut's collider is under its drawn 0.56-0.65: pushed at a run the rig holds its centre
	// 0.9-1.1 m from his feet (logs/beachfeel/push_overlay.txt), which a 0.55 collider held at 1.2.
	private static (float Radius, float RestHeight, bool Rolls, float PushAccel, float Damping)? PushableKind(string key) => key switch
	{
		"act_wumpa_nut" => (0.45f, 0.5615f, true, 50.0f, 0.9f),
		// ponytail: the ball and rock reuse the nut's push/decay until measured; rest heights are the
		// rig's live centres (ball 0.594, rock 0.28).
		"act_beach_ball" => (0.67f, 0.594f, true, 50.0f, 0.9f),
		"act_monkey_rock" => (0.45f, 0.28f, true, 50.0f, 0.9f),
		// The cannon's radius is only its push-contact reach (it collides with its hulls): the body box
		// is 1.5 m half-wide, so Crash's centre touches it ~1.9 m out.
		"act_rigid_cannon" => (1.8f, 1.3f, false, 80.0f, 0.0f),
		// Hay bale and barrel (COM_GLOBAL_HAYBALE_DEFAULT / COM_GLOBAL_BARREL_DEFAULT: SetContactRigid),
		// rig logs/beachfeel/rig_push_hay_*.csv (beach L3 bale, state orig_huba): held like the nut,
		// centres 1.3-1.5 m apart at his run speed; let go it slides to a stop decaying at ~2.5/s. It
		// does not roll, a spin only rocks it (logs/audit/push_rig_hayspin.csv), and a glancing walk
		// slides Crash round it. Both sit their centre RestHeight above the ground: the bale's model
		// half-height 0.72 (rig: placed 0.687 on flat grass, kept), the barrel 0.91 (rig: it drops from
		// its placed 1.62). A learned height (centre minus the rim ground at spawn) read ~0 wherever a
		// rim ray found a surface level with the centre, and the bale then sank 0.9 m.
		// ponytail: the barrel reuses the bale's push/decay - on the rig only glancing bumps (it moved
		// 0.5 m) were captured. Upgrade path: a head-on barrel push with logs/beachfeel/rigpush.py.
		"act_global_haybale" => (1.05f, 0.72f, false, 50.0f, 2.5f),
		"act_global_barrel" => (1.0f, 0.91f, false, 50.0f, 2.5f),
		// act_GLOBAL_BOMB (COM_GLOBAL_BOMB_DEFAULT: SetContactRigid, SetLogicalRadius 0.6): Crash rolls it
		// by walking into it, at his own speed (rig logs/beachcomplete/rig_bomb_roll_sheet.png: pushed
		// 5.6 m in ~1 s into the totem), and it stops soon after he lets go. Rest 0.596 is its live
		// centre (logs/audit/bomb_rig.txt). ponytail: the free-roll decay reuses the bale's 5.6/s (the
		// rig frames show it stopping within ~1 m); upgrade path: track it in EE RAM while released.
		"act_global_bomb" => (0.6f, 0.596f, true, 80.0f, 5.6f),
		_ => null,
	};

	private bool TrySpawnPushable(string objectName, string model, Vector3 position, Vector3 eulerDegrees)
	{
		string key = NameKey(objectName);
		var kind = PushableKind(key);
		if (kind == null)
		{
			return false;
		}

		Entity e = World.Create();
		e.Name = objectName;
		e.AddTransform();
		e.EulerDegrees = eulerDegrees;
		e.LoadModel(model);
		RegisterPushable(e, objectName, key, kind.Value, position, eulerDegrees, model);
		return true;
	}

	// The prefab bind: the model entity is the placed instance root; its collision body, hulls and (for
	// the cannon) button are runtime state, created here exactly as a spawn would. `model` is the
	// descriptor's model path (the cannon's hulls.json hangs off it).
	private bool TryBindPushable(Entity e, string objectName, Vector3 position, Vector3 eulerDegrees, string? model)
	{
		string key = NameKey(objectName);
		var kind = PushableKind(key);
		if (kind == null)
		{
			return false;
		}
		e.EulerDegrees = eulerDegrees;
		RegisterPushable(e, objectName, key, kind.Value, position, eulerDegrees, model);
		return true;
	}

	private void RegisterPushable(Entity e, string objectName, string key, (float Radius, float RestHeight, bool Rolls, float PushAccel, float Damping) kind, Vector3 position, Vector3 eulerDegrees, string? model)
	{
		var (radius, rest, rolls, accel, damping) = kind;

		// The balls, the nut, the bale and the barrel sit their centre RestHeight above the ground
		// (all modelled around their centre); the cannon keeps its placed height and origin.
		bool pivots = key == "act_rigid_cannon";
		float? found = Ground(position, radius, position.Y + 1.0f, default, default, out _);
		float ground = found ?? position.Y - rest;
		Vector3 center = pivots ? position + new Vector3(0.0f, 0.3f, 0.0f) : new Vector3(position.X, ground + rest, position.Z);
		Vector3 modelOffset = pivots ? position - center : Vector3.Zero;
		e.Position = center + modelOffset;

		Entity body = World.Create();
		body.Name = objectName + " Body";
		body.AddTransform();
		body.Position = center;
		var hulls = new List<Entity>();
		Entity button = default;
		if (key == "act_rigid_cannon")
		{
			// The cannon turns in place, so it collides with its own disc hulls (the trunnion
			// bracket, the barrel, the trail plate, its rear box and the handles), turned with it.
			string? text = model != null ? Assets.ReadText(model.Substring(0, model.Length - ".gltf".Length) + ".hulls.json") : null;
			if (text != null)
			{
				using System.Text.Json.JsonDocument doc = System.Text.Json.JsonDocument.Parse(text);
				foreach (System.Text.Json.JsonElement row in doc.RootElement.GetProperty("hulls").EnumerateArray())
				{
					Entity h = HullBody(e, row.GetProperty("rest").GetString()!);
					Physics.SetMotionType(h, PhysicsMotionType.Kinematic);
					hulls.Add(h);
				}
			}

			// The red button on the trail's rear box ("BELLY-FLOP ON THE RED BUTTON"): the disc
			// links it to the cannon as object 779. It turns with the cannon and is solid.
			button = World.Create();
			button.Name = objectName + " Button";
			button.AddTransform();
			button.LoadModel(CannonButtonModel);
		}
		if (hulls.Count == 0)
		{
			Physics.AddSphereBody(body, radius, dynamic: false);
			Physics.SetMotionType(body, PhysicsMotionType.Kinematic);
		}

		var pushable = new Pushable
		{
			Model = e,
			Body = body,
			Center = center,
			ModelOffset = modelOffset,
			Radius = radius,
			RestHeight = center.Y - ground,
			Settled = found != null || pivots || rolls,
			Rolls = rolls,
			PushAccel = accel,
			Damping = damping,
			FreeDamping = damping,
			Base = Quaternion.CreateFromYawPitchRoll(eulerDegrees.Y * MathF.PI / 180.0f, eulerDegrees.X * MathF.PI / 180.0f, eulerDegrees.Z * MathF.PI / 180.0f),
			Pivots = pivots,
			Hulls = hulls,
			Button = button,
			NoLaunch = key == "act_global_bomb",
		};
		if (button.IsValid)
		{
			SyncButton(pushable);
			pushable.ButtonHull = HullBody(button, CannonButtonHull);
			Physics.SetMotionType(pushable.ButtonHull, PhysicsMotionType.Kinematic);
		}
		_pushables.Add(pushable);
		if (pushable.NoLaunch)
		{
			_bombs.Add(new Bomb { Push = pushable, Home = center });
		}
	}

	private void UpdatePushables(float dt, CrashPlayer player)
	{
		if (!player.Self.IsValid || dt <= 0.0f)
		{
			return;
		}
		bool pushing = false;
		Vector3 feet = player.Self.Position;
		Vector3 flatVel = player.Velocity with { Y = 0.0f };
		float speed = flatVel.Length();
		Vector3 moved = _hasLastFeet ? ((feet - _lastFeet) / dt) with { Y = 0.0f } : flatVel;
		// A warp or respawn is not motion: restart the average from his commanded velocity, and let
		// go of whatever he held (else the standing hold drags it after him).
		bool warped = moved.Length() > 30.0f;
		_smoothVel = warped ? flatVel : _smoothVel + (moved - _smoothVel) * (1.0f - MathF.Exp(-dt / ReleaseSmoothing));
		_smoothCmd += (flatVel - _smoothCmd) * (1.0f - MathF.Exp(-dt / PushSmoothing));
		_lastFeet = feet;
		_hasLastFeet = true;
		float smoothSpeed = _smoothVel.Length();
		float cmdSpeed = _smoothCmd.Length();
		Vector3 pushDir = cmdSpeed > 1e-3f ? _smoothCmd / cmdSpeed : Vector3.Zero;
		Vector3 cmdDir = speed > 0.5f ? flatVel / speed : Vector3.Zero;
		bool sharpTurn = cmdDir != Vector3.Zero && _lastCmdDir != Vector3.Zero
			&& MathF.Acos(Math.Clamp(Vector3.Dot(cmdDir, _lastCmdDir), -1.0f, 1.0f)) * 180.0f / MathF.PI > TurnReleaseRate * dt;
		_lastCmdDir = cmdDir;
		// How long the current spin has run: its launch waits SpinLaunchDelay into it. One launch
		// per spin, one per slide.
		_spinClock = player.IsSpinning ? (_spinClock < 0.0f ? 0.0f : _spinClock + dt) : -1.0f;
		_spinLaunched &= _spinClock >= 0.0f;
		_slideLaunched &= player.IsSliding;

		foreach (Pushable p in _pushables)
		{
			if (p.Pivots)
			{
				pushing |= TurnCannon(p, feet, player.IsGrounded && speed > 0.5f ? flatVel : Vector3.Zero, dt);
				continue;
			}
			Vector3 toObj = (p.Center - feet) with { Y = 0.0f };
			float dist = toObj.Length();
			bool overlapY = feet.Y < p.Center.Y + p.Radius * 0.8f && feet.Y + CrashHeight > p.Center.Y - p.Radius;
			bool pushed = false;
			bool contact = dist < p.Radius + CrashRadius + 0.2f && dist > 1e-3f && overlapY && !p.Airborne;
			bool slideHit = player.IsSliding && !_slideLaunched;
			if (contact && p.Rolls && !p.NoLaunch && (slideHit || (_spinClock >= SpinLaunchDelay && !_spinLaunched)))
			{
				Launch(p, toObj / dist, slideHit);
				_slideLaunched |= slideHit;
				_spinLaunched |= !slideHit;
				Step(p, dt, player.Self);
				continue;
			}
			// A jump lets go and the object stays where it is: it does not coast off at his run speed
			// (a take-off beside the bale shoved it back down the totem's tongue).
			bool moving = player.IsGrounded && speed > 0.5f;
			if (p.Held && !player.IsGrounded)
			{
				p.Held = false;
				p.Velocity = Vector3.Zero;
			}
			if (p.Held && (warped || p.Airborne || !overlapY || dist > p.Radius + CrashRadius + HoldRelease))
			{
				p.Held = false;
			}
			if (moving && dist > 1e-3f)
			{
				float cos = Vector3.Dot(flatVel, toObj) / (speed * dist);
				float smoothCos = smoothSpeed > 1e-3f ? Vector3.Dot(_smoothVel, toObj) / (smoothSpeed * dist) : 1.0f;
				float offLine = MathF.Abs(flatVel.X * toObj.Z - flatVel.Z * toObj.X) / speed;
				if (!p.Held && contact && !sharpTurn && cos > GrabCos && offLine < GrabOffset)
				{
					p.Held = true;
				}
				else if (p.Held && (sharpTurn || (smoothSpeed > ReleaseSpeed && smoothCos < ReleaseCos)))
				{
					p.Held = false;
				}
				if (p.Held)
				{
					p.HoldDir = pushDir;
				}
			}
			if (p.Held)
			{
				// Servo to the point just ahead of him along his push line: his velocity (swung
				// round over PushSmoothing) plus the offset closed at CenteringRate, reached at
				// PushAccel. Standing still, the offset alone pulls it back against him (the rig's
				// overshoot-and-return).
				Vector3 hold = feet + p.HoldDir * (p.Radius + CrashRadius + HoldGap);
				Vector3 delta = (moving ? pushDir * speed : Vector3.Zero) + (hold - p.Center) * CenteringRate - p.Velocity;
				delta.Y = 0.0f;
				float max = p.PushAccel * dt;
				p.Velocity += delta.Length() > max ? Vector3.Normalize(delta) * max : delta;
				pushed = true;
				pushing |= moving;
				p.Pushed |= moving;
				p.FreeDamping = p.Damping;
			}
			if (!pushed)
			{
				p.Velocity *= p.Damping > 0.0f ? MathF.Exp(-p.FreeDamping * dt) : 0.0f;
			}
			Step(p, dt, player.Self);
		}
		player.Pushing = pushing;
	}

	// The cannon turns where Crash walks into its hulls, not at a radius about its pivot: on the rig
	// he stands between the handles and runs out through one (the old radius contact never reached
	// the handles 5-6.4 m out, eng_turn.json). The contact follows his tangential motion, capped.
	private static bool TurnCannon(Pushable p, Vector3 feet, Vector3 vel, float dt)
	{
		bool contact = false;
		float speed = vel.Length();
		if (speed > 0.0f)
		{
			// Waist height (the handles sit 0.8-1.4 m above the sand he stands on), from past his
			// capsule and its padding (at +0.01 the ray hit him every frame); a hull he is already
			// pressed against contains the origin and reports there.
			Vector3 dir = vel / speed;
			RaycastHit hit = Physics.Raycast(feet + new Vector3(0.0f, 1.0f, 0.0f) + dir * (CrashRadius + 0.12f), dir, 0.2f);
			if (hit.DidHit && p.Hulls.Contains(hit.Entity))
			{
				// Engine yaw is atan2(x, z): d(yaw)/dt = (r x v) / |r|^2 at the contact.
				Vector3 r = (hit.Position - p.Model.Position) with { Y = 0.0f };
				float w = (vel.X * r.Z - vel.Z * r.X) / MathF.Max(r.LengthSquared(), 1.0f) * 180.0f / MathF.PI;
				p.YawRate = Math.Clamp(w, -PivotMaxRate, PivotMaxRate);
				contact = true;
			}
		}
		if (!contact)
		{
			p.YawRate *= MathF.Exp(-PivotCoastDecay * dt);
			if (MathF.Abs(p.YawRate) < 0.05f)
			{
				p.YawRate = 0.0f;
				return false;
			}
		}
		p.Model.EulerDegrees = p.Model.EulerDegrees with { Y = p.Model.EulerDegrees.Y + p.YawRate * dt };
		foreach (Entity h in p.Hulls)
		{
			h.EulerDegrees = p.Model.EulerDegrees;
		}
		SyncButton(p);
		return contact;
	}

	// The button (and its hull) ride the cannon's yaw about its pivot.
	private static void SyncButton(Pushable p)
	{
		p.Button.Position = p.Model.Position + Yawed(CannonButtonLocal, p.Model.EulerDegrees.Y);
		p.Button.EulerDegrees = p.Model.EulerDegrees;
		if (p.ButtonHull.IsValid)
		{
			p.ButtonHull.Position = p.Button.Position;
			p.ButtonHull.EulerDegrees = p.Button.EulerDegrees;
		}
	}

	// Spin: straight out along the contact normal with a small hop. Slide: thrown up in an arc.
	private static void Launch(Pushable p, Vector3 normal, bool slide)
	{
		p.Velocity = normal * (slide ? SlideLaunchSpeed : SpinLaunchSpeed);
		p.VelocityY = slide ? SlideLaunchUp : SpinLaunchHop;
		p.FreeDamping = slide ? p.Damping : SpinLaunchDamping;
		p.LaunchFloor = p.Center.Y - p.RestHeight;
		if (slide)
		{
			p.Center.Y += SlideLaunchPop;
		}
		p.Airborne = true;
	}

	private void Step(Pushable p, float dt, Entity crash)
	{
		if (p.Airborne)
		{
			Fly(p, dt, crash);
			return;
		}
		float? groundHere = Ground(p.Center, p.Radius, p.Center.Y + 1.0f, p.Body, crash, out Vector2 grad);
		if (!p.Settled && groundHere != null)
		{
			// Spawned before its chunk's collision answered rays: settle onto the ground now.
			p.Settled = true;
			Place(p, p.Center with { Y = groundHere.Value + p.RestHeight }, Vector3.Zero, 0.0f);
		}
		if (p.Rolls && groundHere != null)
		{
			// Downhill pull on a rolling sphere. Gradient is clamped: a rim ray that clipped
			// something tall (a character, a crate) must not fling the object.
			grad = Vector2.Clamp(grad, new Vector2(-0.5f), new Vector2(0.5f));
			p.Velocity -= new Vector3(grad.X, 0.0f, grad.Y) * SlopeGravity * dt;
		}
		if (p.Velocity.LengthSquared() < RestSpeed * RestSpeed)
		{
			p.Velocity = Vector3.Zero;
			p.FreeDamping = p.Damping;
			// A roller spawned before its chunk's collision loaded rests at its instance height
			// (the nut hung 0.45 m up): settle it once the ground is there.
			if (p.Rolls && groundHere != null && MathF.Abs(groundHere.Value + p.RestHeight - p.Center.Y) > 0.02f)
			{
				Place(p, p.Center with { Y = groundHere.Value + p.RestHeight }, Vector3.Zero, 0.0f);
			}
			return;
		}
		Vector3 step = p.Velocity * dt;
		float len = step.Length();
		Vector3 dir = step / len;
		// Stop at scenery: probe ahead from just outside the body so the ray cannot hit it.
		RaycastHit wall = Physics.Raycast(p.Center + dir * (p.Radius + 0.02f), dir, len + 0.05f);
		if (wall.DidHit && wall.Entity != p.Body && wall.Normal.Y < 0.7f)
		{
			p.Velocity = Vector3.Zero;
			return;
		}
		Vector3 next = p.Center + step;
		float? ground = Ground(next, p.Radius, next.Y + 1.0f, p.Body, crash, out _);
		if (ground == null || ground.Value < p.Center.Y - p.RestHeight - 1.0f)
		{
			// ponytail: no falling sim - a pushable stops at a drop instead of going over it.
			p.Velocity = Vector3.Zero;
			return;
		}
		next.Y = ground.Value + p.RestHeight;
		Place(p, next, dir, len);
	}

	// A launched object flies under LaunchGravity until it comes down onto the ground at its rest
	// height; its horizontal speed keeps decaying (UpdatePushables), as the rig's arc does.
	private void Fly(Pushable p, float dt, Entity crash)
	{
		p.VelocityY -= LaunchGravity * dt;
		Vector3 step = p.Velocity * dt;
		float len = step.Length();
		Vector3 dir = len > 1e-5f ? step / len : Vector3.Zero;
		if (len > 1e-5f)
		{
			RaycastHit wall = Physics.Raycast(p.Center + dir * (p.Radius + 0.02f), dir, len + 0.05f);
			if (wall.DidHit && wall.Entity != p.Body && wall.Normal.Y < 0.7f)
			{
				p.Velocity = Vector3.Zero;
				step = Vector3.Zero;
				len = 0.0f;
			}
		}
		Vector3 next = p.Center + step + new Vector3(0.0f, p.VelocityY * dt, 0.0f);
		// The arc tops out ~3 m up: probe well below it. ponytail: over a void (nothing within 40 m)
		// it lands level with the ground it left - there is no falling sim below the launch.
		float floor = Ground(next, p.Radius, MathF.Max(next.Y, p.Center.Y) + 1.0f, p.Body, crash, out _, 40.0f) ?? p.LaunchFloor;
		if (p.VelocityY < 0.0f && next.Y <= floor + p.RestHeight)
		{
			next.Y = floor + p.RestHeight;
			p.VelocityY = 0.0f;
			p.Airborne = false;
		}
		Place(p, next, dir, len);
	}

	private static void Place(Pushable p, Vector3 next, Vector3 dir, float len)
	{
		p.Center = next;
		p.Body.Position = next;
		p.Model.Position = next + p.ModelOffset;
		if (p.Rolls && len > 1e-5f)
		{
			Vector3 axis = Vector3.Normalize(Vector3.Cross(Vector3.UnitY, dir));
			// A rolling sphere turns by its rest radius (its centre height), not its collider.
			p.Roll = Quaternion.Normalize(Quaternion.Concatenate(p.Roll, Quaternion.CreateFromAxisAngle(axis, len / p.RestHeight)));
			p.Model.EulerDegrees = EngineEuler(Matrix4x4.CreateFromQuaternion(Quaternion.Concatenate(p.Base, p.Roll)));
		}
	}

	// Ground under a body: rays down just outside its rim, +x -x +z -z. A hit on the body itself (the
	// kinematic body trails its target by a physics step) or anything above its centre (Crash's
	// capsule) is not ground; a hit level with the centre is (the bomb's instance puts its centre on
	// the ground, and it must still find that ground to settle its rest height above it).
	// Returns the highest hit and the height gradient (dh/dx, dh/dz).
	// ponytail: four rim samples - a ball resting on a crest between them reads slightly low; upgrade
	// to a shape cast that can filter the body out.
	private static float? Ground(Vector3 center, float radius, float fromY, Entity self, Entity crash, out Vector2 grad, float reach = 1.5f)
	{
		float r = radius + 0.05f;
		Span<float> h = stackalloc float[4];
		Span<Vector2> offsets = stackalloc Vector2[] { new(r, 0), new(-r, 0), new(0, r), new(0, -r) };
		float? best = null;
		for (int i = 0; i < 4; i++)
		{
			h[i] = float.NaN;
			RaycastHit hit = Physics.Raycast(new Vector3(center.X + offsets[i].X, fromY, center.Z + offsets[i].Y), -Vector3.UnitY, fromY - center.Y + radius + reach);
			bool ignored = hit.Entity == self || hit.Entity == crash;
			if (hit.DidHit && !ignored && hit.Position.Y < center.Y + 0.05f)
			{
				h[i] = hit.Position.Y;
				best = best == null ? h[i] : MathF.Max(best.Value, h[i]);
			}
		}
		grad = new Vector2(
			float.IsNaN(h[0]) || float.IsNaN(h[1]) ? 0.0f : (h[0] - h[1]) / (2.0f * r),
			float.IsNaN(h[2]) || float.IsNaN(h[3]) ? 0.0f : (h[2] - h[3]) / (2.0f * r));
		return best;
	}

	// Row-vector rotation -> the engine's Euler degrees (same convention as TwinsanityLevel.EulerOf).
	private static Vector3 EngineEuler(Matrix4x4 m)
	{
		const float deg = 180.0f / MathF.PI;
		float x = MathF.Asin(Math.Clamp(-m.M32, -1.0f, 1.0f));
		float y = MathF.Atan2(m.M31, m.M33);
		float z = MathF.Atan2(m.M12, m.M22);
		return new Vector3(x, y, z) * deg;
	}
}
