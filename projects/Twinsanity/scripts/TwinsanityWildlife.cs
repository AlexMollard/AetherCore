using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Per-creature behaviours measured on the original (PCSX2 rig, logs/wildlife/*.csv: live actor
/// positions read from EE RAM at 10 Hz) and read from the RM2 behaviour scripts
/// (logs/wildlife/scripts*.txt). Distances quoted as "script" are the script's squared-distance
/// thresholds (MeToPlayerSqrDist) rooted; "rig" values are measured.
///
/// - Seagull: stands and hops around its perch; when Crash comes within 15.5 m (rig) it climbs at
///   4.97 m/s (4.45 up, 2.2 across) to 12 m above the ground, then circles a 3.15 m radius at
///   1.556 rad/s for good. Instances with flag 0x80000 start already circling at perch height.
/// - Butterfly: straight legs at 3 m/s around its spawn, now and then drops straight down to
///   rest on the ground for 7-10 s.
/// - Bird clump: a flock of birds flying the clump's path at 19.7 m/s.
/// - Chicken: spawned by act_CREATURE_SPAWNER (the coop) at its AI points; pecks about, walks
///   1 m/s bursts, bolts from Crash inside 5 m and calms beyond 7 m (script 25 / 49).
/// - Crab: act_UTIL_ECOLOGY_MANAGER spawns five at the key nearest Crash once he is within
///   80 m (script 6400; counter 40 in steps of 8). They lie dormant under the sand until he is
///   within 20 m (COM_GLOBAL_CRAB_INIT, sqr 400), rise 2 m at 2 m/s, and a crab charges at once
///   when he is within 5 m (COM_GLOBAL_CRAB_ROAM, sqr 25; rig rig_crab_notice.csv) - touching
///   him hurts.
/// - Path crab (huba old_act_GLOBAL_CRAB4, the same object 180, subtypes 1/2): stands at its
///   spot until Crash is within √180 m or trigger 1 sends message 87 (COM_GLOBAL_CRAB_INIT
///   S10/S11), then walks its two route keys for good (COM_GLOBAL_CRAB_IDLE). It never charges;
///   touching it hurts and his attacks kill it like any crab.
/// - Skunk: patrols its instance path (COM_CREATURE_BASIC_IDLE_PATROL) at the instance's walk
///   speed (floats[2], 2.2 m/s), pausing ~1.3 s to turn at each end; it never chases Crash
///   (rig: logs/audit/skunk_patrol_rig.csv). Touching it hurts; his attacks kill it.
/// - Worm: fixed in place, 2.5 m under its hole; pops up when Crash is within 14.1 m and sinks
///   beyond 13.4 m (script 200 / 180). Landing on a popped worm launches Crash (Mechanics).
/// - Monkey: when Crash is within 20.6 m (script 425) it climbs its wumpa tree, shakes a fruit
///   down, climbs down, picks the fruit up and throws it at Crash (rig: 20 m/s across, 0.7 s
///   flight for 14 m, a hit costs a mask).
/// </summary>
public sealed partial class TwinsanityActors
{
	private readonly System.Random _rng = new(0x7715);
	private readonly List<Vector3> _monkeyTrees = new();
	private readonly Dictionary<string, SpawnInfo> _instances = new();
	private readonly Dictionary<string, Actor> _byInstance = new();
	private readonly List<Spawner> _spawners = new();

	// Seagull (rig: logs/wildlife/track_takeoff.csv, track_b0.csv).
	private const float GullTakeOffRadius = 15.5f;
	private const float GullClimbUp = 4.45f;
	private const float GullClimbAcross = 2.2f;
	private const float GullCircleHeight = 12.0f;
	private const float GullCircleRadius = 3.15f;
	private const float GullCircleOmega = 1.556f;
	// Rig (logs/wildlife/gull2_rig.csv, gull2_rig_flight.csv; Crash standing still): ground gulls
	// step 0.1-1.75 m at 1.2-1.5 m/s for 0.4-1.2 s, 5-30 s apart (mean ~12 s), and now and then
	// take off with nothing near them (g0: Crash 21 m away and still) into the usual 12 m circle,
	// which they then hold (none landed within 100 s).
	private const float GullWalk = 1.35f;
	// ponytail: one unprovoked takeoff in ~300 gull-seconds of watching; the mean wait is that
	// single sample. Upgrade path: a longer rig watch (or the script's timer) for a real rate.
	private const float GullRandomTakeOffMean = 300.0f;
	// Butterfly (rig: track_b0.csv).
	private const float ButterflySpeed = 3.0f;
	private const float ButterflyFlee = 6.0f;
	// Bird clump (rig: track_b0.csv k1-k3).
	private const float FlockSpeed = 19.7f;
	// Chicken (script COM_GLOBAL_CHICKEN_DEFAULT; rig track_long.csv).
	private const float ChickenPanic = 5.0f;
	private const float ChickenCalm = 7.0f;
	private const float ChickenWalk = 1.0f;
	// Rig (logs/wildlife/chicken_flee_rig.csv): Crash ran at a coop chicken; it bolted at
	// 4.6-5.1 m/s (1.85 m in 0.40 s; 1.27 m in 0.25 s mid-run) and stopped at 7.0 m from him.
	private const float ChickenFlee = 5.0f;
	// Crab (script COM_GLOBAL_CRAB_*; rig track_crab2.csv, crab2_wander.csv, logs/gameplay/
	// rig_crab_notice.csv: all five rose the moment Crash came within 20 m, the one 3.9 m from him
	// charged within a frame, one 6.9 m away never noticed him).
	private const float CrabShuttleStop = 10.3f;
	private const float CrabShuttleSpeed = 2.3f;
	private const float CrabShuttleWait = 5.0f;
	private const float CrabWake = 20.0f;
	private const float CrabBurrow = 2.0f;
	private const float CrabRise = 2.0f;
	private const float CrabRiseWait = 0.5f;
	private const float CrabNotice = 5.0f;
	private const float CrabSpeed = 2.7f;
	private const float CrabGiveUp = 20.0f;
	// Path crab (rig logs/tutorialfinal/rig_crab4_wake.csv, crabs 9 and 10 over 30 s): the instance's
	// floats[2] (3.4 m/s) between the keys, stopping ~0.5 m short of each; 1.0 s standing at key 1,
	// 0.5 s at key 0. First leg: to the key farther from its spot (crab 9 went north, crab 10 south).
	private const float PathCrabWakeSqr = 180.0f;
	private const float PathCrabArrive = 0.5f;
	private const float PathCrabPauseKey0 = 0.5f;
	private const float PathCrabPauseKey1 = 1.0f;
	// Skunk (rig: logs/audit/skunk_patrol_rig.csv). Walk speed is the instance's floats[2].
	private const float SkunkWalk = 2.2f;
	private const float SkunkArrive = 0.36f;
	private const float SkunkTurnPause = 1.3f;
	// Worm (script COM_EARTH_WORM_START / _SQUASHLAUNCH / _SLAMMED / _MOVE, rig track_worm.csv,
	// logs/gameplay/rig_worm_slam.csv).
	// START S10 (down) pops on NOT MeToPlayerSqrDist 180; S9 (up) sinks on MeToPlayerSqrDist 200.
	private const float WormPopRadius = 13.42f;
	private const float WormHideRadius = 14.14f;
	private const float WormDepth = 2.5f;
	private const float WormSquashDip = 1.2f;    // SQUASHLAUNCH: RAWPOS_Y -1.2 then +1.2 ...
	private const float WormSquashSpeed = 7.6f;  // ... at MOVE_SPEED 7.6 (rig: root 6.18 -> 4.98 -> 6.18)
	private const float WormSinkSpeed = 16.66f;  // MOVE S3: down 2.5 m
	private const float WormTravelSpeed = 13.0f; // MOVE S4/S12: to the next key, 2 m under
	private const float WormRiseSpeed = 10.0f;   // MOVE S7: up 2.5 m
	private const float WormLoneWait = 1.0f;     // MOVE S5: a worm with no other hole waits 1 s
	private const float WormAwayDwell = 10.0f;   // START S9 TimeInUnit 10 away from its first key
	// Monkey (script COM_GLOBAL_MONKEY_ECOLOGY_IDLE, rig track_monkey.csv).
	private const float MonkeyAggro = 20.6f;
	private const float MonkeyMinRange = 4.0f;
	private const float MonkeyWalk = 1.9f;
	private const float MonkeyClimbUp = 5.0f;
	private const float MonkeyClimbDown = 4.0f;
	private const float MonkeyTreeTop = 5.58f;
	private const float FruitAcross = 20.0f;
	private const float FruitGravity = 42.0f;

	private sealed class SpawnInfo
	{
		public int ObjectId;
		public string Name = "";
		public string? Model;
		public Vector3 Euler;
		public float[] Floats = Array.Empty<float>();
	}

	private sealed class Spawner
	{
		public bool Ecology;
		public string TemplateKey = "";
		public Vector3 Position;
		public List<Vector3> Points = new();
		public int Count;
		public bool Resolved;
		public bool[] KeyUsed = Array.Empty<bool>();
		public SpawnInfo? Template;
		public List<Actor> Live = new();
		public float Refill = -1.0f;
	}

	private enum Mode { Idle, Walk, Flee, Climb, Circle, Land, Rest, Charge, Up, Down, GoTree, ClimbUp, Shake, ClimbDown, GoFruit, Pickup, Throw, Sink, Travel }

	private sealed class Critter
	{
		public Mode Mode;
		public float Timer;
		public Vector3 Target;
		public Vector3 Center;
		public float Sign = 1.0f;
		public List<Vector3> Points = new();
		public int PathIndex;
		public Vector3 Offset;
		public Vector3 Tree;
		public Entity Fruit;           // the fruit in play (dropped, carried or thrown)
		public Vector3 FruitVelocity;
		public bool FruitFlying;
		public bool Landed;
		public float FruitLife;
		public float Notice, GiveUp;
		public float Timer2;
		public float Speed;
		public float Dwell;     // worm: time up since it came out of a hole after a move
		public bool AfterMove;  // worm: idling in START S0 after a move (no hide check)
		public int FlyClip = -1, ClimbClip = -1, WalkClip = -1, RestClip = -1, PopClip = -1, SinkClip = -1, SquashClip = -1, SpunClip = -1, ThrowClip = -1, PickClip = -1, ShakeClip = -1, BiteClip = -1;
	}

	// Instance ids are per chunk layer, and links point within the layer.
	private static string ChunkKey(Matrix4x4 t, JsonElement instance, int id)
		=> $"{t.M41:F2},{t.M42:F2},{t.M43:F2}#{(instance.TryGetProperty("layer", out JsonElement l) ? l.GetInt32() : 0)}#{id}";

	private static Vector3 Vec3(JsonElement e) => new(e[0].GetSingle(), e[1].GetSingle(), e[2].GetSingle());

	private static uint Flags(JsonElement instance) => instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("flags", out JsonElement f) ? f.GetUInt32() : 0u;

	private static List<Vector3> PointList(JsonElement instance, string name, Matrix4x4 transform)
	{
		var list = new List<Vector3>();
		if (instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty(name, out JsonElement arr))
		{
			foreach (JsonElement p in arr.EnumerateArray())
			{
				list.Add(Vector3.Transform(Vec3(p), transform));
			}
		}
		return list;
	}

	// ---- classification (shared with TwinsanityBake, which dispatches on these) ----

	// Marker roles TwinsanityBake assigns actor-side things; must match TwinsanityBake's constants.
	internal const int SpawnerCreature = 4;
	internal const int SpawnerParrot = 5;

	// Name-based spawner classification without side effects. True spawner-hood also needs the
	// instance shape (a creature spawner carries links); TryRegisterSpawner checks that part.
	internal static int SpawnerRoleFor(string objectName)
	{
		string n = NameKey(objectName);
		if (n.StartsWith("act_parrot_spawner"))
		{
			return SpawnerParrot;
		}
		if (n.StartsWith("act_creature_spawner") || n.StartsWith("act_util_ecology_manager"))
		{
			return SpawnerCreature;
		}
		return 0;
	}

	// The bake skips exactly what play-time spawning skips; internal so both share the rule.
	internal static bool Skipped(string objectName) => ShouldSkip(objectName);

	// Lower-case, family name with trailing instance numbers stripped. (The bake reads it to
	// mirror the wumpa-tree shadow rule.)
	internal static string NameKeyOf(string objectName) => NameKey(objectName);

	// Creature spawners and the ecology manager have no model: they spawn copies of the instance
	// they link. True when this instance is one (it is then consumed).
	private bool TryRegisterSpawner(string objectName, JsonElement instance, Matrix4x4 transform, Vector3 position)
	{
		int role = SpawnerRoleFor(objectName);
		if (role == SpawnerParrot)
		{
			_parrotSpawners.Add(new ParrotSpawner
			{
				Position = position,
				Keys = PointList(instance, "points", transform),
				Count = instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("params", out JsonElement pp) && pp.GetArrayLength() > 2 ? pp[2].GetInt32() : 1,
			});
			return true;
		}
		if (role != SpawnerCreature || instance.ValueKind != JsonValueKind.Object || !instance.TryGetProperty("links", out JsonElement links))
		{
			return false;
		}
		var s = new Spawner
		{
			Ecology = NameKey(objectName).StartsWith("act_util_ecology_manager"),
			TemplateKey = ChunkKey(transform, instance, links[0].GetInt32()),
			Position = position,
			Points = PointList(instance, "points", transform),
		};
		// Coop: the instance's own count (params[2], 4 at the beach coop - 4 chickens on the rig).
		// Ecology: COM_UTIL_ECOLOGY_MANAGER_DEFAULT sets counter 40 and spends 8 per spawn.
		s.Count = NameKey(objectName).StartsWith("act_util_ecology_manager") ? 5 : (instance.TryGetProperty("params", out JsonElement pr) && pr.GetArrayLength() > 2 ? pr[2].GetInt32() : 1);
		s.KeyUsed = new bool[Math.Max(1, s.Points.Count)];
		_spawners.Add(s);
		return true;
	}

	private void RememberInstance(JsonElement instance, Matrix4x4 transform, int objectId, string objectName, string? model, Vector3 euler, float[] floats)
	{
		if (instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("id", out JsonElement id))
		{
			_instances[ChunkKey(transform, instance, id.GetInt32())] = new SpawnInfo { ObjectId = objectId, Name = objectName, Model = model, Euler = euler, Floats = floats };
		}
	}

	private void TrackInstance(JsonElement instance, Matrix4x4 transform, Actor a)
	{
		if (instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("id", out JsonElement id))
		{
			_byInstance[ChunkKey(transform, instance, id.GetInt32())] = a;
		}
	}

	// Set up a creature's per-kind state after its model and clips are loaded.
	private void SetupCritter(Actor a, string objectName, JsonElement instance, Matrix4x4 transform)
	{
		Entity e = a.Model;
		var c = new Critter();
		a.Critter = c;
		switch (a.Kind)
		{
			case Behaviour.Seagull:
				// Clips: a001 stand, a002 hop, a011/a012 flight loops (a011 flapping climb, a012 glide).
				c.WalkClip = Animation.Find(e, "a002");
				c.ClimbClip = Animation.Find(e, "a011");
				c.FlyClip = Animation.Find(e, "a012");
				if ((Flags(instance) & 0x80000u) != 0)
				{
					// Already airborne: circles at perch height around a point ~1.41 m off the perch
					// (rig: g47/g50/g51).
					float ang = (float)(_rng.NextDouble() * Math.PI * 2.0);
					c.Center = a.Home + new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang)) * 1.41f;
					c.Sign = _rng.Next(2) == 0 ? 1.0f : -1.0f;
					c.Timer = (float)(_rng.NextDouble() * Math.PI * 2.0);
					c.Mode = Mode.Circle;
					PlayClip(a, c.FlyClip);
				}
				else
				{
					c.Mode = Mode.Idle;
					c.Timer = RandomRange(3.0f, 15.0f);
					c.Timer2 = -GullRandomTakeOffMean * MathF.Log(1.0f - (float)_rng.NextDouble());
				}
				break;
			case Behaviour.Butterfly:
				c.FlyClip = Animation.Find(e, "a001");
				c.RestClip = Animation.Find(e, "a008");
				c.Mode = Mode.Walk;
				c.Target = ButterflyTarget(a);
				PlayClip(a, c.FlyClip);
				break;
			case Behaviour.Flock:
				c.Points = PointList(instance, "path", transform);
				c.FlyClip = Animation.Find(e, "a007");
				PlayClip(a, c.FlyClip);
				break;
			case Behaviour.Chicken:
				// OGI slots (clip motion, logs/chickenpush): a001 stand, a007 peck (neck bobs, legs still),
				// a002 walk, a008 flee (fast legs, flapping wings); COM_GLOBAL_CHICKEN_HIT plays a010.
				c.WalkClip = Animation.Find(e, "a002");
				c.PickClip = Animation.Find(e, "a007");
				c.FlyClip = Animation.Find(e, "a008");
				a.DeathClip = Animation.Find(e, "a010");
				c.Mode = Mode.Idle;
				// Rig: the coop chickens are usually on the move (2 of 4 mid-step in any 2 s sample),
				// while a 1-10 s idle parked ours on the peck clip - read as "the feet don't move".
				c.Timer = RandomRange(1.0f, 4.0f);
				break;
			case Behaviour.Crab:
				// Dormant under the sand until Crash comes near (COM_GLOBAL_CRAB_INIT S17).
				c.WalkClip = Animation.Find(e, "a002");
				c.Notice = CrabNotice;
				c.GiveUp = CrabGiveUp;
				c.Mode = Mode.Down;
				if (SubtypeOf(instance) is 1 or 2 && PointList(instance, "points", transform) is { Count: > 1 } keys)
				{
					// Path crab: stands at its spot, visible, until woken (INIT S10/S11).
					c.Points = keys;
					float[] floats = FloatsOf(instance);
					c.Speed = floats.Length > 2 ? floats[2] : CrabShuttleSpeed;
					c.PathIndex = Horizontal(a.Home, keys[0]) > Horizontal(a.Home, keys[1]) ? 0 : 1;
					PlayClip(a, a.IdleClip);
					break;
				}
				a.Model.Position = a.Home - new Vector3(0.0f, CrabBurrow, 0.0f);
				a.Model.SetActive(false);
				break;
			case Behaviour.Skunk:
				// COM_CREATURE_BASIC_IDLE_PATROL: a002 walk to the next route key, a001 while turning.
				// Rig (logs/audit/skunk_patrol_rig.csv, huba skunk 12): 2.15 m/s legs (the instance's
				// floats[2] = 2.2), stops ~0.36 m short of each key and stands 1.3 s before heading
				// back - a ping-pong along the keys, never off them.
				c.WalkClip = Animation.Find(e, "a002");
				c.Points = PointList(instance, "points", transform);
				c.Speed = SkunkWalk;
				if (instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("floats", out JsonElement fl) && fl.GetArrayLength() > 2)
				{
					c.Speed = fl[2].GetSingle();
				}
				c.Mode = Mode.Walk;
				break;
			case Behaviour.Piranha:
				// COM_PIRANHAPLANT_DEFAULT: a001 idle, a007 rears up on noticing Crash, a010 watching,
				// a011 bite; COM_PIRANHAPLANT_DAMAGED plays a004 and holds it (knocked flat for good).
				c.PopClip = Animation.Find(e, "a007");
				c.RestClip = Animation.Find(e, "a010");
				c.ThrowClip = Animation.Find(e, "a011");
				a.DeathClip = Animation.Find(e, "a004");
				c.Mode = Mode.Idle;
				break;
			case Behaviour.Worm:
				// COM_EARTH_WORM_START: a005 pops up (S10 before the S11 rise), a006 sinks (S12 before
				// the S13 drop); IDLE: a002 idle loop (S0), a003 bite (S3); a012 squash-launch, a011 spun.
				// Its holes are the instance's points.
				a.IdleClip = Animation.Find(e, "a002");
				c.PopClip = Animation.Find(e, "a005");
				c.SinkClip = Animation.Find(e, "a006");
				c.BiteClip = Animation.Find(e, "a003");
				c.SquashClip = Animation.Find(e, "a012");
				c.SpunClip = Animation.Find(e, "a011");
				c.Points = PointList(instance, "points", transform);
				c.Timer2 = -1.0f;
				c.Mode = Mode.Down;
				a.Model.Position = a.Home - new Vector3(0.0f, WormDepth, 0.0f);
				a.Angle = a.HomeYaw;
				break;
			case Behaviour.Monkey:
				// Clips from the monkey scripts: a002 walk, a003 pick up (ECOLOGY_IDLE s5), a022
				// climb/shake (DO_TREE s3/s6), a029 throw (ECOLOGY_IDLE s14).
				c.WalkClip = Animation.Find(e, "a002");
				c.PickClip = Animation.Find(e, "a003");
				c.ShakeClip = Animation.Find(e, "a022");
				c.ClimbClip = Animation.Find(e, "a023");
				c.ThrowClip = Animation.Find(e, "a029");
				c.Mode = Mode.Idle;
				// Rig: the three monkeys cycle out of phase (throws ~25-35 s apart); a shared start
				// made all three fruit land at once.
				c.Timer = RandomRange(2.0f, 30.0f);
				break;
		}
	}

	private float RandomRange(float lo, float hi) => lo + (float)_rng.NextDouble() * (hi - lo);

	private static float GroundY(Vector3 p, float fallback, float above = 2.0f)
	{
		RaycastHit hit = Physics.Raycast(p + new Vector3(0.0f, above, 0.0f), -Vector3.UnitY, 40.0f);
		return hit.DidHit ? hit.Position.Y : fallback;
	}

	private static float Horizontal(Vector3 a, Vector3 b)
	{
		float dx = a.X - b.X, dz = a.Z - b.Z;
		return MathF.Sqrt(dx * dx + dz * dz);
	}

	// Moves toward target at speed on the ground plane; true on arrival.
	private bool WalkTo(Actor a, Vector3 target, float speed, float dt, bool followGround)
	{
		Vector3 p = a.Model.Position;
		Vector3 d = new(target.X - p.X, 0.0f, target.Z - p.Z);
		float len = d.Length();
		float step = speed * dt;
		Vector3 next = len <= step ? new Vector3(target.X, p.Y, target.Z) : p + d / len * step;
		if (followGround)
		{
			// From just above the feet: a walker under a roof (chickens in the coop) stays on the floor.
			next.Y = GroundY(next, p.Y, 0.6f);
		}
		a.Model.Position = next;
		if (len > 0.001f)
		{
			FaceMovement(a, d);
		}
		return len <= step;
	}

	private void UpdateCritters()
	{
		// Resolve spawners once their template instance is known (chunks load in any order).
		foreach (Spawner s in _spawners)
		{
			if (s.Resolved || !_instances.TryGetValue(s.TemplateKey, out SpawnInfo? info))
			{
				continue;
			}
			s.Resolved = true;
			if (_byInstance.TryGetValue(s.TemplateKey, out Actor? template))
			{
				// The template never lives in the world itself: it is only copied.
				template.Alive = false;
				template.Model.Destroy();
			}
			// The two spawners behind the waterfall copy act_EARTH_FISHFOUNTAIN_* (COM_EARTH_LEMMING:
			// warp near the spawner, thrown as a rigid body, splash and DestroyMe). On the rig nothing
			// comes out of them in free roam - Crash stood 6 m away on the waterfall ledge for 12 s
			// and no fish agent existed (logs/wildlife/fish_rig_1.png, fish_rig_2.png) - so the port
			// spawns none rather than parking 14 static fish in the rock.
			if (NameKey(info.Name).StartsWith("act_earth_fishfountain"))
			{
				_spawners.Remove(s);
				break;
			}
			if (!s.Ecology)
			{
				for (int i = 0; i < s.Count; i++)
				{
					Vector3 at = s.Points.Count > 0 ? s.Points[i % s.Points.Count] : s.Position;
					Actor? c = SpawnCopy(info, at);
					if (c == null)
					{
						continue;
					}
					if (c.Critter != null)
					{
						c.Critter.Points = s.Points;
					}
					s.Live.Add(c);
				}
			}
			s.Template = info;
		}
	}

	private Actor? SpawnCopy(SpawnInfo info, Vector3 at)
	{
		if (info.Model == null)
		{
			return null;
		}
		Entity e = World.Create();
		e.Name = info.Name + "#copy";
		e.AddTransform();
		e.Position = new Vector3(at.X, GroundY(at, at.Y), at.Z);
		e.EulerDegrees = info.Euler;
		e.LoadModel(info.Model);
		Actor a = new() { Model = e, Home = e.Position, HomeYaw = info.Euler.Y };
		ReadClips(e, a);
		a.Kind = BehaviourOf(info.Name);
		SetLooping(e, true);
		if (a.IdleClip >= 0)
		{
			Animation.SetClip(e, a.IdleClip);
		}
		SetupCritter(a, info.Name, default, Matrix4x4.Identity);
		_pending.Add(a);
		return a;
	}

	private readonly List<Actor> _pending = new();

	// Ground creatures never overlap (rig: crabs and chickens keep their spacing). Circle push,
	// half the overlap per side, horizontally only. Rooted ones (piranhas, worms in their holes) never
	// move: a passing critter had shoved beach worm 2 0.9 m off its hole.
	private void Separate()
	{
		for (int i = 0; i < _actors.Count; i++)
		{
			Actor? x = _actors[i];
			if (!x.Alive || x.Critter == null || x.DeathTimer >= 0.0f || x.Kind is Behaviour.Piranha or Behaviour.Worm)
			{
				continue;
			}
			for (int j = i + 1; j < _actors.Count; j++)
			{
				Actor? y = _actors[j];
				if (!y.Alive || y.Critter == null || y.DeathTimer >= 0.0f || y.Kind is Behaviour.Piranha or Behaviour.Worm)
				{
					continue;
				}
				float radii = CritterRadius(x) + CritterRadius(y);
				Vector3 d = y.Model.Position - x.Model.Position;
				d.Y = 0.0f;
				float len = d.Length();
				if (len >= radii || len < 0.0001f)
				{
					continue;
				}
				Vector3 push = d / len * (radii - len) * 0.5f;
				x.Model.Position -= push;
				y.Model.Position += push;
			}
		}
	}

	private static float CritterRadius(Actor a) => a.Kind switch
	{
		Behaviour.Chicken => 0.35f,
		Behaviour.Crab or Behaviour.Monkey => 0.45f,
		_ => 0.5f,
	};

	// Blast damage for ground creatures (TwinsanityLevel.Explode calls this).
	public void CreatureBlast(Vector3 center, float radius)
	{
		foreach (Actor a in _actors)
		{
			if (!a.Alive || a.Critter == null || a.DeathTimer >= 0.0f || (a.Kind == Behaviour.Crab && a.Critter.Mode == Mode.Down))
			{
				continue;
			}
			Vector3 p = a.Model.Position;
			if (new Vector2(center.X - p.X, center.Z - p.Z).Length() < radius && MathF.Abs(center.Y - p.Y) < radius)
			{
				if (a.Kind == Behaviour.Piranha)
				{
					if (a.Critter?.Mode != Mode.Rest)
					{
						KnockFlat(a, a.Critter);
					}
					continue;
				}
				Kill(a, center);
			}
		}
	}

	// A spin, slide or slam reaches a spun chicken 1.58 m out (rig logs/gameplay/rig_chicken_spin.csv).
	private const float AttackReach = 1.6f;

	/// <summary>Crash's attacks kill a ground creature in any state - dormant, idle, wandering,
	/// fleeing, charging or up its tree: a spin, slide or slam within reach, or landing on it.
	/// True when it died.</summary>
	private bool Attacked(Actor a, Vector3 crashPos)
	{
		CrashPlayer? player = _player;
		if (player == null)
		{
			return false;
		}
		Vector3 p = a.Model.Position;
		// A crab still under the sand or rising out of it counts from its key.
		float y = a.Kind == Behaviour.Crab ? MathF.Max(p.Y, a.Home.Y) : p.Y;
		if (crashPos.Y >= y + 1.6f || crashPos.Y + 1.8f <= y)
		{
			return false;
		}
		float h = Horizontal(p, crashPos);
		bool hit = (h < AttackReach && (player.IsSpinning || player.IsSliding || player.IsSlamming))
			|| (h < EnemyHitRadius && player.Velocity.Y < -2.0f && crashPos.Y > y + 0.8f);
		if (hit)
		{
			a.Model.SetActive(true);
			Kill(a, crashPos);
		}
		return hit;
	}

	/// <summary>Death: the rig's knockback arc away from <paramref name="from"/> (Knockback.cs), the
	/// death clip once, tumbling while airborne; it lies Arc.Linger once at rest, then goes.</summary>
	private void Kill(Actor a, Vector3 from)
	{
		Vector3 p = a.Model.Position;
		a.Arc = a.Kind == Behaviour.Chicken ? Knockback.Chicken : Knockback.Creature;
		float yaw = a.Model.EulerDegrees.Y * MathF.PI / 180.0f;
		a.FlyVelocity = Knockback.Launch(from, p, a.Arc, -new Vector3(MathF.Sin(yaw), 0.0f, MathF.Cos(yaw)));
		a.Flying = true;
		a.Bounced = false;
		a.DeathTimer = a.Arc.Linger;
		// It flies back from the hit, facing it.
		a.FlyYaw = MathF.Atan2(-a.FlyVelocity.X, -a.FlyVelocity.Z) * (180.0f / MathF.PI);
		a.Tumble = 0.0f;
		a.Model.EulerDegrees = new Vector3(0.0f, a.FlyYaw, 0.0f);
		PlayClip(a, a.DeathClip >= 0 ? a.DeathClip : a.MoveClip);
		SetLooping(a.Model, false);
		if (a.Critter is Critter c && c.Fruit.IsValid)
		{
			c.Fruit.Destroy(); // a monkey's fruit, in hand or in the air, goes with it
			c.FruitFlying = false;
		}
		TwinsanityAudio.Creature(TwinsanityAudio.Call.Death, p);
	}

	// One step of a death knockback; tumbles backward about the body's side axis while airborne.
	private void Fly(Actor a, float dt)
	{
		Vector3 p = a.Model.Position, v = a.FlyVelocity;
		bool bounced = a.Bounced;
		bool rest = Knockback.Step(ref p, ref v, ref bounced, a.Arc, GroundY(p, a.Home.Y, 0.6f), dt);
		a.Model.Position = p;
		a.FlyVelocity = v;
		a.Bounced = bounced;
		a.Flying = !rest;
		a.Tumble = rest ? 0.0f : a.Tumble - a.Arc.Tumble * dt;
		a.Model.EulerDegrees = new Vector3(a.Tumble * (180.0f / MathF.PI), a.FlyYaw, 0.0f);
	}

	// Piranha plant (COM_PIRANHAPLANT_DEFAULT, rig logs/audit/piranha_rig_*): rooted. Crash inside
	// 14.1 m (MeToPlayerSqrDist 200) makes it rear up (a007) and watch him (a010), turned to face
	// him; inside 5.48 m (sqr 30) it bites (a011) over and over, ~2 s a bite. The bite's
	// CreateDamage lands 0.48 s in (the script's 0.25 + 0.23 s waits) and reaches ~3 m: on the rig
	// a bite at 5 m misses, at 2.8 m it takes a mask and knocks him back. A spin, slide or slam
	// knocks it flat (a004, held) for good - it stays in the world, harmless.
	private const float PiranhaNotice = 14.14f;
	private const float PiranhaBite = 5.48f;
	private const float PiranhaBiteHitAt = 0.48f;
	// ponytail: the bite reach is bracketed by two rig samples (miss at 5 m, hit at 2.8 m); 3.2 m
	// sits inside. Upgrade path: step Crash out from 2.8 m on the rig until a bite misses.
	private const float PiranhaReach = 3.2f;
	private const float PiranhaSpinReach = 2.0f;

	private void UpdatePiranha(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		if (c.Mode == Mode.Rest)
		{
			return;
		}
		Vector3 p = a.Model.Position;
		float d = Horizontal(p, crashPos);
		CrashPlayer? player = _player;
		if (player != null && d < PiranhaSpinReach && MathF.Abs(crashPos.Y - p.Y) < 2.0f
			&& (player.IsSpinning || player.IsSliding || player.IsSlamming))
		{
			KnockFlat(a, c);
			return;
		}
		if (c.Mode != Mode.Idle && d > 0.01f)
		{
			FaceMovement(a, new Vector3(crashPos.X - p.X, 0.0f, crashPos.Z - p.Z));
		}
		switch (c.Mode)
		{
			case Mode.Idle:
				PlayClip(a, a.IdleClip);
				if (d < PiranhaNotice)
				{
					c.Mode = Mode.Up;
					c.Timer = 1.32f; // a007
					PlayClip(a, c.PopClip);
				}
				break;
			case Mode.Up:
				c.Timer -= dt;
				if (c.Timer <= 0.0f)
				{
					c.Mode = Mode.Circle;
				}
				break;
			case Mode.Circle:
				PlayClip(a, c.RestClip);
				if (d >= PiranhaNotice)
				{
					c.Mode = Mode.Idle;
				}
				else if (d < PiranhaBite)
				{
					c.Mode = Mode.Throw;
					c.Timer = 0.0f;
					c.Landed = false;
					Animation.CrossFade(a.Model, c.ThrowClip, 0.1f);
				}
				break;
			case Mode.Throw:
				c.Timer += dt;
				if (!c.Landed && c.Timer >= PiranhaBiteHitAt)
				{
					c.Landed = true;
					if (d < PiranhaReach && MathF.Abs(crashPos.Y - p.Y) < 2.0f)
					{
						_host?.DamagePlayer(p, DeathKind.Generic);
					}
				}
				if (c.Timer >= 1.92f) // a011
				{
					c.Mode = Mode.Circle;
				}
				break;
		}
	}

	private static void KnockFlat(Actor a, Critter? c)
	{
		if (c != null)
		{
			c.Mode = Mode.Rest;
		}
		SetLooping(a.Model, false);
		if (a.DeathClip >= 0)
		{
			Animation.CrossFade(a.Model, a.DeathClip, 0.1f);
		}
		TwinsanityAudio.Creature(TwinsanityAudio.Call.Death, a.Model.Position);
	}

	/// <summary>Crash respawned at a zone checkpoint: a piranha plant spun flat stands again, as on the rig
	/// (logs/hubb rig_w3_sheet.png: flattened, then standing after the death at the swinging log).</summary>
	public void ResetPiranhas()
	{
		foreach (Actor a in _actors)
		{
			if (a.Kind == Behaviour.Piranha && a.Critter is { Mode: Mode.Rest } c)
			{
				c.Mode = Mode.Idle;
				a.Model.EulerDegrees = new Vector3(0.0f, a.HomeYaw, 0.0f);
				SetLooping(a.Model, true);
				PlayClip(a, a.IdleClip);
			}
		}
	}

	// Ground creatures set off nitro crates on contact (TwinsanityLevel implements the crate side).
	private void TouchHost()
	{
		foreach (Actor a in _actors)
		{
			if (a.Alive && a.Critter != null && a.DeathTimer < 0.0f
				&& a.Kind is Behaviour.Chicken or Behaviour.Crab or Behaviour.Skunk or Behaviour.Monkey)
			{
				_host?.CreatureTouch(a.Model.Position);
			}
		}
	}

	private void UpdateEcology(Vector3 crashPos, float dt)
	{
		foreach (Spawner s in _spawners)
		{
			if (!s.Ecology && s.Template != null)
			{
				RefillCoop(s, dt);
				continue;
			}
			if (!s.Ecology || s.Template == null || s.Points.Count == 0)
			{
				continue;
			}
			int best = -1;
			float bestDist = float.MaxValue;
			for (int i = 0; i < s.Points.Count; i++)
			{
				float d = Vector3.Distance(s.Points[i], crashPos);
				if (d < bestDist)
				{
					bestDist = d;
					best = i;
				}
			}
			if (best < 0 || s.KeyUsed[best] || bestDist >= 80.0f)
			{
				continue;
			}
			s.KeyUsed[best] = true;
			for (int i = 0; i < s.Count; i++)
			{
				// Rig: the five crabs sat within ~3 m of the key.
				float ang = RandomRange(0.0f, MathF.PI * 2.0f), r = RandomRange(0.0f, 3.0f);
				SpawnCopy(s.Template, s.Points[best] + new Vector3(MathF.Cos(ang) * r, 0.0f, MathF.Sin(ang) * r));
			}
		}
	}

	// The coop keeps its count alive. Rig (logs/wildlife/coop_respawn_rig.csv): a chicken that dies
	// (spun, or walking into a nitro) is replaced ~2 s later (1-3 s between 1 s RAM samples, twice)
	// at the spawner itself - the coop hatch, (12.44, 0.2, -4.0) - and walks out from there.
	private const float CoopRefillDelay = 2.0f;

	private void RefillCoop(Spawner s, float dt)
	{
		s.Live.RemoveAll(a => !a.Alive);
		if (s.Live.Count >= s.Count)
		{
			s.Refill = -1.0f;
			return;
		}
		if (s.Refill < 0.0f)
		{
			s.Refill = CoopRefillDelay;
		}
		s.Refill -= dt;
		if (s.Refill > 0.0f)
		{
			return;
		}
		s.Refill = -1.0f;
		Actor? c = SpawnCopy(s.Template!, s.Position);
		if (c == null)
		{
			return;
		}
		s.Live.Add(c);
		if (c.Critter != null)
		{
			// Inside the coop: the ground ray from above would land on its roof.
			c.Model.Position = s.Position;
			c.Home = s.Position;
			c.Critter.Points = s.Points;
		}
	}

	// Parrots (COM_PARROT_SPAWNER_DEFAULT spawning ENV_PARROT; rig: logs/wildlife/parrot_rig.csv,
	// huba spawners at 26-28 m from Crash). The spawner puts params[2] parrots on its keys once Crash
	// is inside sqrt(800) = 28.3 m and clears them past sqrt(1000) = 31.6 m. Each parrot perches near
	// a key for 1-6 s (rig: airborne ~70% of the time, perches of 1-10 s), then flies to another
	// of its spawner's keys at 5.46 m/s (rig median while moving),
	// arcing 3-6 m above the keys, and perches again within ~3 m of it (the script's key noise).
	private const float ParrotNear = 28.3f;
	private const float ParrotFar = 31.6f;
	private const float ParrotSpeed = 5.46f;
	// ponytail: arc height, perch offset and clip choice are read off one 40 s rig trace, not the
	// script; upgrade path: decode ENV_PARROT's COM_ENV_PARROT_START focus/anim commands.
	private const float ParrotArc = 4.0f;
	private const float ParrotPerchLift = 0.7f;
	private const string ParrotModel = "project://assets/models/objects/ENV_PARROT/ENV_PARROT.gltf";

	private sealed class ParrotSpawner
	{
		public Vector3 Position;
		public List<Vector3> Keys = new();
		public int Count;
		public List<Actor> Live = new();
	}

	private readonly List<ParrotSpawner> _parrotSpawners = new();

	private void UpdateParrotSpawners(Vector3 crashPos)
	{
		foreach (ParrotSpawner s in _parrotSpawners)
		{
			float d = Vector3.Distance(s.Position, crashPos);
			if (s.Live.Count == 0 && d < ParrotNear && s.Keys.Count > 0)
			{
				for (int i = 0; i < s.Count; i++)
				{
					s.Live.Add(SpawnParrot(s, i % s.Keys.Count));
				}
			}
			else if (s.Live.Count > 0 && d > ParrotFar)
			{
				foreach (Actor a in s.Live)
				{
					a.Alive = false;
					a.Model.Destroy();
				}
				s.Live.Clear();
			}
		}
	}

	private Actor SpawnParrot(ParrotSpawner s, int key)
	{
		Entity e = World.Create();
		e.Name = "ENV_PARROT#spawned";
		e.AddTransform();
		e.Position = s.Keys[key] + new Vector3(0.0f, ParrotPerchLift, 0.0f);
		e.LoadModel(ParrotModel);
		Actor a = new() { Model = e, Home = e.Position, Kind = Behaviour.Parrot };
		ReadClips(e, a);
		SetLooping(e, true);
		var c = new Critter
		{
			Points = s.Keys,
			PathIndex = key,
			Mode = Mode.Rest,
			Timer = RandomRange(0.0f, 8.0f),
			FlyClip = Animation.Find(e, "a011"),
			RestClip = Animation.Find(e, "a001"),
		};
		a.Critter = c;
		PlayClip(a, c.RestClip);
		_pending.Add(a);
		return a;
	}

	private void UpdateParrot(Actor a, Critter c, float dt)
	{
		switch (c.Mode)
		{
			case Mode.Rest:
				c.Timer -= dt;
				if (c.Timer <= 0.0f && c.Points.Count > 1)
				{
					int next = (c.PathIndex + 1 + _rng.Next(c.Points.Count - 1)) % c.Points.Count;
					float ang = RandomRange(0.0f, MathF.PI * 2.0f), r = RandomRange(0.0f, 3.0f);
					c.PathIndex = next;
					c.Center = a.Model.Position;
					c.Target = c.Points[next] + new Vector3(MathF.Cos(ang) * r, ParrotPerchLift, MathF.Sin(ang) * r);
					c.Timer = 0.0f;
					c.Timer2 = MathF.Max(0.5f, Vector3.Distance(c.Center, c.Target) / ParrotSpeed);
					c.Mode = Mode.Walk;
					PlayClip(a, c.FlyClip);
				}
				break;
			case Mode.Walk:
			{
				c.Timer += dt;
				float t = MathF.Min(1.0f, c.Timer / c.Timer2);
				Vector3 next = Vector3.Lerp(c.Center, c.Target, t) + new Vector3(0.0f, ParrotArc * 4.0f * t * (1.0f - t), 0.0f);
				FaceMovement(a, next - a.Model.Position);
				a.Model.Position = next;
				if (t >= 1.0f)
				{
					c.Mode = Mode.Rest;
					c.Timer = RandomRange(1.0f, 6.0f);
					PlayClip(a, c.RestClip);
				}
				break;
			}
		}
	}

	private void UpdateSeagull(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		Vector3 p = a.Model.Position;
		switch (c.Mode)
		{
			case Mode.Idle:
			case Mode.Walk:
				// Rig: a gull 4.4 m from a standing Crash stayed put for minutes, while ones Crash
				// walked toward left at 15.5 m (gull_approach_rig.csv: take-off at 16.2 m into a slow
				// walk) - it takes a moving Crash to scare them, and he can never reach one.
				Vector3 cv = _player?.Velocity ?? Vector3.Zero;
				c.Timer2 -= dt;
				bool scare = Horizontal(p, crashPos) < GullTakeOffRadius && cv.X * cv.X + cv.Z * cv.Z > 0.02f;
				if (scare || c.Timer2 <= 0.0f)
				{
					float ang = RandomRange(0.0f, MathF.PI * 2.0f);
					c.Target = new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang));
					c.Mode = Mode.Climb;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.GullTakeOff, a.Model.Position);
					PlayClip(a, c.ClimbClip);
					FaceMovement(a, c.Target);
					break;
				}
				if (c.Mode == Mode.Idle)
				{
					PlayClip(a, a.IdleClip);
					c.Timer -= dt;
					if (c.Timer <= 0.0f)
					{
						// Rig g0-g2: short steps about the perch (they stayed within ~2 m of it).
						float ang = RandomRange(0.0f, MathF.PI * 2.0f);
						c.Target = a.Home + new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang)) * RandomRange(0.0f, 2.0f);
						c.Mode = Mode.Walk;
						c.Timer = RandomRange(0.4f, 1.2f);
					}
				}
				else
				{
					PlayClip(a, c.WalkClip >= 0 ? c.WalkClip : a.IdleClip);
					c.Timer -= dt;
					if (WalkTo(a, c.Target, GullWalk, dt, true) || c.Timer <= 0.0f)
					{
						c.Mode = Mode.Idle;
						c.Timer = RandomRange(5.0f, 20.0f);
					}
				}
				break;
			case Mode.Climb:
			{
				Vector3 v = c.Target * GullClimbAcross + new Vector3(0.0f, GullClimbUp, 0.0f);
				p += v * dt;
				a.Model.Position = p;
				// Rig: g7 and g45 levelled off at exactly 12.00 over flat sand, the others 12-14.5
				// above their perch - consistent with 12 m above the ground beneath them.
				if (p.Y - GroundY(p, a.Home.Y) >= GullCircleHeight)
				{
					// Level off into a circle tangent to the climb heading.
					c.Sign = _rng.Next(2) == 0 ? 1.0f : -1.0f;
					Vector3 side = new(-c.Target.Z * c.Sign, 0.0f, c.Target.X * c.Sign);
					c.Center = new Vector3(p.X, p.Y, p.Z) + side * GullCircleRadius;
					c.Timer = MathF.Atan2(p.Z - c.Center.Z, p.X - c.Center.X);
					c.Mode = Mode.Circle;
					PlayClip(a, c.FlyClip);
				}
				break;
			}
			case Mode.Circle:
			{
				c.Timer += c.Sign * GullCircleOmega * dt;
				Vector3 next = c.Center + new Vector3(MathF.Cos(c.Timer), 0.0f, MathF.Sin(c.Timer)) * GullCircleRadius;
				FaceMovement(a, next - p);
				a.Model.Position = next;
				break;
			}
		}
	}

	private Vector3 ButterflyTarget(Actor a)
	{
		// Never let Crash touch one: rig approaches always ended with the butterfly well away from
		// him. ponytail: the trigger is a single muddy rig approach (butterflies range widely);
		// 6 m and the measured 3 m/s are the working numbers. Upgrade path: a cleaner rig capture.
		if (_player != null)
		{
			Vector3 away = a.Model.Position - _player.Self.Position;
			away.Y = 0.0f;
			float d = away.Length();
			if (d < ButterflyFlee)
			{
				return a.Home + (d > 1e-3f ? away / d : new Vector3(0.0f, 0.0f, 1.0f)) * (ButterflyFlee + 2.0f);
			}
		}
		// Rig: legs spread ~15 m around the spawn, 0-6 m above the ground.
		float ang = RandomRange(0.0f, MathF.PI * 2.0f), r = RandomRange(0.0f, 7.5f);
		Vector3 t = a.Home + new Vector3(MathF.Cos(ang) * r, 0.0f, MathF.Sin(ang) * r);
		t.Y = GroundY(t, a.Home.Y) + RandomRange(0.5f, 6.0f);
		return t;
	}

	private void UpdateButterfly(Actor a, Critter c, float dt)
	{
		Vector3 p = a.Model.Position;
		switch (c.Mode)
		{
			case Mode.Walk:
			{
				Vector3 d = c.Target - p;
				float len = d.Length(), step = ButterflySpeed * dt;
				if (len <= step)
				{
					a.Model.Position = c.Target;
					if (_rng.NextDouble() < 0.3)
					{
						c.Mode = Mode.Land;
						c.Timer = GroundY(c.Target, a.Home.Y);
					}
					else
					{
						c.Target = ButterflyTarget(a);
					}
					break;
				}
				a.Model.Position = p + d / len * step;
				FaceMovement(a, d);
				break;
			}
			case Mode.Land:
				// Rig: straight down at ~3 m/s onto the ground.
				p.Y -= ButterflySpeed * dt;
				if (p.Y <= c.Timer)
				{
					p.Y = c.Timer;
					c.Mode = Mode.Rest;
					c.Timer = RandomRange(7.0f, 10.0f);
					PlayClip(a, c.RestClip >= 0 ? c.RestClip : c.FlyClip);
				}
				a.Model.Position = p;
				break;
			case Mode.Rest:
				c.Timer -= dt;
				if (c.Timer <= 0.0f)
				{
					c.Mode = Mode.Walk;
					c.Target = ButterflyTarget(a);
					PlayClip(a, c.FlyClip);
				}
				break;
		}
	}

	private void UpdateFlock(Actor a, Critter c, float dt)
	{
		if (c.Points.Count < 2)
		{
			return;
		}
		Vector3 p = a.Model.Position;
		Vector3 target = c.Points[c.PathIndex] + c.Offset;
		Vector3 d = target - p;
		float len = d.Length(), step = FlockSpeed * dt;
		if (len <= step)
		{
			c.PathIndex = (c.PathIndex + 1) % c.Points.Count;
			a.Model.Position = target;
			return;
		}
		a.Model.Position = p + d / len * step;
		FaceMovement(a, d);
	}

	// The clump itself flies as one of its birds; the rest are copies spread about it (rig: 16
	// bird agents shared the beach clump's home, spread over ~10 m while flying).
	private void SpawnFlock(Actor clump, string model)
	{
		Critter lead = clump.Critter!;
		lead.Offset = new Vector3(RandomRange(-5.0f, 5.0f), RandomRange(-2.0f, 2.0f), RandomRange(-5.0f, 5.0f));
		for (int i = 1; i < 16; i++)
		{
			Entity e = World.Create();
			e.Name = clump.Model.Name + "#bird" + i;
			e.AddTransform();
			Vector3 offset = new(RandomRange(-5.0f, 5.0f), RandomRange(-2.0f, 2.0f), RandomRange(-5.0f, 5.0f));
			e.Position = clump.Home + offset;
			e.LoadModel(model);
			Actor a = new() { Model = e, Home = clump.Home, Kind = Behaviour.Flock };
			ReadClips(e, a);
			SetLooping(e, true);
			var c = new Critter { Points = lead.Points, Offset = offset, FlyClip = Animation.Find(e, "a007") };
			a.Critter = c;
			PlayClip(a, c.FlyClip);
			_actors.Add(a);
		}
	}

	private void UpdateChicken(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		Vector3 p = a.Model.Position;
		float d = Horizontal(p, crashPos);
		if (d < ChickenPanic)
		{
			c.Mode = Mode.Flee;
		}
		if (c.Mode == Mode.Flee)
		{
			if (d > ChickenCalm)
			{
				c.Mode = Mode.Idle;
				c.Timer = RandomRange(1.0f, 4.0f);
				return;
			}
			Vector3 away = d > 0.001f ? new Vector3(p.X - crashPos.X, 0.0f, p.Z - crashPos.Z) / d : Vector3.UnitZ;
			PlayClip(a, c.FlyClip >= 0 ? c.FlyClip : c.WalkClip);
			WalkTo(a, p + away * 2.0f, ChickenFlee, dt, true);
			return;
		}
		if (c.Mode == Mode.Idle)
		{
			// Rig: idle chickens peck most of the time, with short stands between bouts.
			c.Timer2 -= dt;
			if (c.Timer2 <= 0.0f)
			{
				c.RestClip = c.RestClip == c.PickClip ? a.IdleClip : c.PickClip;
				c.Timer2 = c.RestClip == c.PickClip ? RandomRange(1.5f, 4.0f) : RandomRange(0.8f, 2.0f);
			}
			PlayClip(a, c.RestClip >= 0 ? c.RestClip : a.IdleClip);
			c.Timer -= dt;
			if (c.Timer <= 0.0f)
			{
				// Rig: ~1.3 s walks at 1 m/s between the coop's AI points, ~10 s apart.
				Vector3 at = c.Points.Count > 0 ? c.Points[_rng.Next(c.Points.Count)] : a.Home;
				c.Target = at + new Vector3(RandomRange(-1.0f, 1.0f), 0.0f, RandomRange(-1.0f, 1.0f));
				c.Mode = Mode.Walk;
				TwinsanityAudio.Creature(TwinsanityAudio.Call.Cluck, a.Model.Position);
				c.Timer = 2.5f;
			}
			return;
		}
		PlayClip(a, c.WalkClip);
		c.Timer -= dt;
		if (WalkTo(a, c.Target, ChickenWalk, dt, true) || c.Timer <= 0.0f)
		{
			c.Mode = Mode.Idle;
			c.Timer = RandomRange(4.0f, 15.0f);
		}
	}

	/// <summary>Trigger message 87 (huba trigger 1 -> crabs 9 and 10) wakes the path crab standing at
	/// <paramref name="home"/> (COM_GLOBAL_CRAB_INIT S11 leaves its wait on it).</summary>
	private void WakePathCrab(Vector3 home)
	{
		foreach (Actor a in _actors)
		{
			if (a.Kind == Behaviour.Crab && a.Critter is { Points.Count: > 1, Mode: Mode.Down } c && Horizontal(a.Home, home) < 0.5f)
			{
				c.Mode = Mode.Walk;
			}
		}
	}

	private void UpdatePathCrab(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		TouchCrash(a, crashPos);
		switch (c.Mode)
		{
			case Mode.Down:
				PlayClip(a, a.IdleClip);
				if (Vector3.DistanceSquared(a.Model.Position, crashPos) < PathCrabWakeSqr)
				{
					c.Mode = Mode.Walk;
				}
				break;
			case Mode.Idle:
				PlayClip(a, a.IdleClip);
				c.Timer -= dt;
				if (c.Timer <= 0.0f)
				{
					c.Mode = Mode.Walk;
				}
				break;
			default:
				PlayClip(a, c.WalkClip);
				Vector3 key = c.Points[c.PathIndex];
				WalkTo(a, key, c.Speed, dt, true);
				if (Horizontal(a.Model.Position, key) <= PathCrabArrive)
				{
					c.Mode = Mode.Idle;
					c.Timer = c.PathIndex == 0 ? PathCrabPauseKey0 : PathCrabPauseKey1;
					c.PathIndex = 1 - c.PathIndex;
				}
				break;
		}
	}

	private void UpdateCrab(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		if (c.Points.Count > 1)
		{
			UpdatePathCrab(a, c, dt, crashPos);
			return;
		}
		Vector3 p = a.Model.Position;
		float d = Horizontal(p, crashPos);
		switch (c.Mode)
		{
			case Mode.Down:
				// Dormant at its key under the sand until Crash is within 20 m (INIT S17, sqr 400).
				if (Vector3.Distance(a.Home, crashPos) < CrabWake)
				{
					c.Mode = Mode.Up;
					c.Timer = 0.0f;
					a.Model.SetActive(true);
					PlayClip(a, a.IdleClip);
				}
				break;
			case Mode.Up:
				// INIT S14: up 2 m at 2 m/s, then S12 waits 0.5 s (rig: -2.0 -> 0.0 in 1.0 s).
				a.Model.Position = new Vector3(p.X, MathF.Min(a.Home.Y, p.Y + CrabRise * dt), p.Z);
				if (a.Model.Position.Y >= a.Home.Y)
				{
					c.Timer += dt;
					if (c.Timer >= CrabRiseWait)
					{
						c.Mode = Mode.Idle;
						c.Timer2 = RandomRange(2.0f, 6.0f);
					}
				}
				break;
			case Mode.Idle:
				PlayClip(a, a.IdleClip);
				if (d < c.Notice)
				{
					// ROAM: Crash inside 5 m (sqr 25) sends it to ATTACK at once (rig: within a frame).
					c.Mode = Mode.Charge;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.CrabCharge, a.Model.Position);
					break;
				}
				// Rig (crab_far_rig.csv, crab_far_rig2.csv; Crash still 21 m from the group, 100 s):
				// crabs farther than 20 m from him stand still. One inside 20 m shuttles: it walks
				// toward him until ~10.3 m away, waits ~5 s, walks back to its key, waits ~5 s, and
				// repeats, at ~2.3 m/s.
				if (d >= CrabWake)
				{
					break;
				}
				c.Timer2 -= dt;
				if (c.Timer2 <= 0.0f)
				{
					bool atKey = Horizontal(p, a.Home) < 0.5f;
					Vector3 toward = new(p.X - crashPos.X, 0.0f, p.Z - crashPos.Z);
					c.Target = atKey && toward.LengthSquared() > 1e-4f
						? crashPos + Vector3.Normalize(toward) * CrabShuttleStop
						: a.Home;
					c.Mode = Mode.Walk;
				}
				break;
			case Mode.Charge:
				if (d > c.GiveUp)
				{
					c.Mode = Mode.Walk;
					c.Target = a.Home;
					break;
				}
				// ponytail: the crab charge speed is the measured 2.7 m/s.
				PlayClip(a, c.ThrowClip >= 0 && d < 4.0f ? c.ThrowClip : c.WalkClip);
				if (d > 0.6f)
				{
					WalkTo(a, crashPos, CrabSpeed, dt, true);
				}
				TouchCrash(a, crashPos);
				break;
			case Mode.Walk:
				PlayClip(a, c.WalkClip);
				if (d < c.Notice)
				{
					c.Mode = Mode.Charge; // ROAM S1 (pathfinding about) notices him just the same
					TwinsanityAudio.Creature(TwinsanityAudio.Call.CrabCharge, a.Model.Position);
					break;
				}
				if (WalkTo(a, c.Target, CrabShuttleSpeed, dt, true))
				{
					c.Mode = Mode.Idle;
					c.Timer2 = CrabShuttleWait;
				}
				break;
		}
	}

	// Ping-pong along the route keys at the instance speed, standing SkunkTurnPause at each end.
	// Crash does not change its route: it only hurts him on contact (or dies to his attacks).
	private void UpdateSkunk(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		TouchCrash(a, crashPos);
		if (a.DeathTimer >= 0.0f)
		{
			return;
		}
		if (c.Points.Count == 0)
		{
			PlayClip(a, a.IdleClip);
			return;
		}
		if (c.Mode == Mode.Idle)
		{
			PlayClip(a, a.IdleClip);
			c.Timer -= dt;
			if (c.Timer <= 0.0f)
			{
				c.Mode = Mode.Walk;
			}
			return;
		}
		PlayClip(a, c.WalkClip >= 0 ? c.WalkClip : a.IdleClip);
		Vector3 key = c.Points[c.PathIndex];
		WalkTo(a, key, c.Speed, dt, true);
		if (Horizontal(a.Model.Position, key) > SkunkArrive)
		{
			return;
		}
		if (c.Points.Count > 1)
		{
			if (c.PathIndex + (int)c.Sign < 0 || c.PathIndex + (int)c.Sign >= c.Points.Count)
			{
				c.Sign = -c.Sign;
				c.Mode = Mode.Idle;
				c.Timer = SkunkTurnPause;
			}
			c.PathIndex += (int)c.Sign;
		}
		else
		{
			c.Mode = Mode.Idle;
			c.Timer = float.MaxValue;
		}
	}

	// Contact with Crash hurts him unless he is attacking (Attacked has already killed it then).
	private void TouchCrash(Actor a, Vector3 crashPos)
	{
		CrashPlayer? player = _player;
		Vector3 p = a.Model.Position;
		if (player == null || Horizontal(p, crashPos) >= EnemyHitRadius || crashPos.Y >= p.Y + 1.6f || crashPos.Y + 1.8f <= p.Y)
		{
			return;
		}
		bool attacking = player.IsSpinning || player.IsSliding || player.IsSlamming
			|| (player.Velocity.Y < -2.0f && crashPos.Y > p.Y + 0.8f);
		if (!attacking)
		{
			_host?.DamagePlayer(p, DeathKind.Generic);
		}
	}

	private void UpdateWorm(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		float d = Vector3.Distance(a.Home, crashPos);
		Vector3 down = a.Home - new Vector3(0.0f, WormDepth, 0.0f);
		Vector3 p = a.Model.Position;
		if (c.Mode != Mode.Up)
		{
			c.Dwell = 0.0f;
		}
		switch (c.Mode)
		{
			case Mode.Down:
				// START S13 (the sink) plays out before S10 can pop it again.
				if (d < WormPopRadius && p.Y <= down.Y)
				{
					c.Mode = Mode.Up;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Model.Position);
					PlayClip(a, c.PopClip);
				}
				// Rig: 3.68 -> 6.18 in 0.25 s up, 0.2 s down.
				a.Model.Position = new Vector3(p.X, MathF.Max(down.Y, p.Y - WormDepth / 0.2f * dt), p.Z);
				break;
			case Mode.Up:
			{
				float y = MathF.Min(a.Home.Y, p.Y + WormDepth / 0.25f * dt);
				if (c.Timer2 >= 0.0f)
				{
					// Squash (SQUASHLAUNCH S0/S1): the root dips 1.2 m and comes back at 7.6 m/s while
					// a012 lifts the body by about as much (joint1 +1.0 m over its first 0.2 s), so the
					// worm stays in its hole - rig: root 6.18 -> 4.98 -> 6.18 in 0.3 s.
					c.Timer2 += dt;
					float half = WormSquashDip / WormSquashSpeed;
					y = a.Home.Y - WormSquashDip * MathF.Max(0.0f, 1.0f - MathF.Abs(c.Timer2 - half) / half);
					if (c.Timer2 >= 2.0f * half)
					{
						c.Timer2 = -1.0f;
					}
				}
				a.Model.Position = new Vector3(p.X, y, p.Z);
				// The pop, squash and spun clips each hand back to the idle once they are over (START S8 ->
				// IDLE S0 a002; SQUASHLAUNCH / SPUN end in RestartDefaultBehaviour). Left looping, a squash
				// kept replaying a012 - the bounce - over a worm that was only standing there.
				int current = Animation.CurrentClip(a.Model);
				if (a.Model.Position.Y >= a.Home.Y && c.Timer <= 0.0f && !c.Landed
					&& (current == c.PopClip || current == c.SquashClip || current == c.SpunClip))
				{
					PlayClip(a, a.IdleClip);
				}
				c.Timer = MathF.Max(0.0f, c.Timer - dt);
				// After a move the worm idles in START S0, which has no hide check, and S0's TimeInUnit 10
				// sends it on to the next key unless it is at its first (rig: worm 35 stayed up 10.3 s at
				// hole 2 with Crash 5 m and 19 m off, then went back to hole 1 and stayed there -
				// logs/hubroute/rig_worm_dwell.csv, rig_worm_dwell2.csv). A squash or a spin restarts
				// START, and so the count.
				if (c.AfterMove && a.Model.Position.Y >= a.Home.Y && !TwinsanityCutscenes.Active)
				{
					c.Dwell += dt;
					if (c.Dwell >= WormAwayDwell)
					{
						c.Dwell = 0.0f;
						if (c.PathIndex != 0)
						{
							c.Mode = Mode.Sink;
							c.Landed = false;
							PlayClip(a, c.SinkClip);
							TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Model.Position);
							break;
						}
						c.AfterMove = false;
					}
				}
				// A scene holds a popped worm up (rig: scene B's worm 35 stays up at hole 2, Crash 31 m off).
				if (d > WormHideRadius && a.Model.Position.Y >= a.Home.Y && !c.AfterMove && !TwinsanityCutscenes.Active)
				{
					c.Mode = Mode.Down;
					c.Timer2 = -1.0f;
					PlayClip(a, c.SinkClip);
					break;
				}
				CrashPlayer? player = _player;
				if (player != null && c.Timer2 < 0.0f && a.Model.Position.Y >= a.Home.Y - 0.1f)
				{
					float h = Horizontal(a.Home, crashPos);
					if (h < 1.3f && player.IsSlamming && crashPos.Y > a.Home.Y + 0.3f && crashPos.Y < a.Home.Y + 2.5f)
					{
						// COM_EARTH_WORM_SLAMMED -> _MOVE: a006, down 2.5 m at 16.66 m/s, then off to
						// its next hole (or back up the same one after 1 s if it has only one).
						c.Mode = Mode.Sink;
						c.Landed = false; // an attack in progress is cut short
						PlayClip(a, c.SinkClip);
						TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Model.Position);
					}
					else if (h < 0.8f && crashPos.Y > a.Home.Y + 0.3f && crashPos.Y <= a.Home.Y + MechanicsWorm.WormTop && MechanicsWorm.TryLaunch(player, a.Home, crashPos.Y))
					{
						PlayClip(a, c.SquashClip);
						c.Landed = false;
						c.Timer = 1.7f;
						c.Timer2 = 0.0f;
						c.Dwell = 0.0f;
					}
					else if (h < 1.3f && player.IsSpinning && c.Timer <= 0.0f)
					{
						PlayClip(a, c.SpunClip);
						c.Landed = false;
						c.Timer = 1.7f;
						c.Dwell = 0.0f;
					}
					else if (c.Timer <= 0.0f)
					{
						WormAttack(a, c, dt, crashPos);
					}
				}
				break;
			}
			case Mode.Sink:
				a.Model.Position = new Vector3(p.X, MathF.Max(down.Y, p.Y - WormSinkSpeed * dt), p.Z);
				if (a.Model.Position.Y > down.Y)
				{
					break;
				}
				if (c.Points.Count > 1)
				{
					// MOVE S4: the next key of its path, 13 m/s under the ground (cmd 36 steps the key).
					c.PathIndex = (c.PathIndex + 1) % c.Points.Count;
					c.Target = c.Points[c.PathIndex];
					c.Mode = Mode.Travel;
				}
				else
				{
					c.Mode = Mode.Rest; // MOVE S5: DELAY 1
					c.Timer = WormLoneWait;
				}
				break;
			case Mode.Travel:
			{
				Vector3 to = c.Target - new Vector3(0.0f, WormDepth, 0.0f);
				Vector3 step = to - p;
				float len = step.Length();
				if (len > WormTravelSpeed * dt)
				{
					a.Model.Position = p + step / len * WormTravelSpeed * dt;
					break;
				}
				a.Model.Position = to;
				a.Home = c.Target;
				Emerge(a, c);
				break;
			}
			case Mode.Rest:
				c.Timer -= dt;
				if (c.Timer <= 0.0f)
				{
					Emerge(a, c);
				}
				break;
		}
	}

	// Worm bite (COM_EARTH_WORM_IDLE S1-S5, logs/wormbite): with Crash inside 3 m (MeToPlayerSqrDist 9, a 3D
	// distance) S3 plays a003 - it rears back with its mouth open, then lunges - with Sounds[8,9], and S5's
	// CreateDamage lands the bite on the lunge: rig, 9-11 frames after the rear starts, which is a003 at 0.3 s
	// (engine pose sheet logs/wormbite/pose_a003_*). No bounce clip: a012 is the squash-launch only. S4 waits
	// the clip out (AnimationFinished) and S1 strikes again at once if he is still inside 3 m.
	// Before the rear the rig worm faces him and waits out its idle loop: 0.14-1.34 s from his arrival 2 m off
	// to the rear over 14 trials on four sides (rig_capture/rig_turn/rig_turnshots.csv). Turning at 180 deg/s
	// (logs/wildlife/worm_turn_rig.txt) and then waiting for the end of the a002 loop spans 0-1.4 s.
	private const float WormAttackRadius = 3.0f;
	private const float WormTurnRate = 180.0f;
	private const float WormHitAt = 0.3f;
	private const float WormBiteClip = 1.12f; // a003

	// c.Landed: biting; c.FruitLife: time into a003; c.Speed: the idle loop's phase last frame.
	private void WormAttack(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		float d = Vector3.Distance(a.Home, crashPos);
		if (c.Landed)
		{
			float before = c.FruitLife;
			c.FruitLife += dt;
			if (before < WormHitAt && c.FruitLife >= WormHitAt && d < WormAttackRadius)
			{
				_host?.DamagePlayer(a.Home, DeathKind.Generic);
			}
			if (c.FruitLife < WormBiteClip)
			{
				return;
			}
			c.Landed = false;
			PlayClip(a, a.IdleClip);
			c.Speed = float.MaxValue; // S4 -> S1 -> S3: no idle loop to wait out before the next bite
		}
		float length = Animation.ClipDuration(a.Model);
		float phase = length > 0.0f ? Animation.GetTime(a.Model) % length : 0.0f;
		bool loopEnd = Animation.CurrentClip(a.Model) != a.IdleClip || phase < c.Speed;
		c.Speed = phase;
		if (d >= WormAttackRadius)
		{
			return;
		}
		// Its own yaw (a.Angle, set to HomeYaw at spawn): EulerDegrees reads back a yaw past 90 as
		// (180, 180 - yaw, 180), so turning from the read-back value started from the wrong heading.
		float want = MathF.Atan2(crashPos.X - a.Home.X, crashPos.Z - a.Home.Z) * (180.0f / MathF.PI);
		float diff = ((want - a.Angle) % 360.0f + 540.0f) % 360.0f - 180.0f;
		float step = WormTurnRate * dt;
		a.Angle = MathF.Abs(diff) <= step ? want : a.Angle + MathF.Sign(diff) * step;
		a.Model.EulerDegrees = new Vector3(0.0f, a.Angle, 0.0f);
		if (MathF.Abs(diff) > step || !loopEnd)
		{
			return;
		}
		c.Landed = true;
		c.FruitLife = 0.0f;
		Animation.CrossFade(a.Model, c.BiteClip, 0.1f); // DoAnim a003, blend 0.1
		TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Home);
	}

	/// <summary>A scene actor's blow on a live worm (Coco's landings in hubb scene B, the impact messages of
	/// COM_COCO_CUTSCENE_L01B's cmd 158): `slam` is 230 -> COM_EARTH_WORM_SLAMMED (sink and move to the next
	/// hole, as Crash's slam), otherwise 226 -> _SQUASHLAUNCH_NOIMPULSE (the squash dip, no launch of Crash).
	/// The nearest worm within 2 m of `at` takes it.</summary>
	public void ScriptedWormHit(Vector3 at, bool slam)
	{
		Actor? a = null;
		float best = 4.0f;
		foreach (Actor x in _actors)
		{
			float d = Horizontal(x.Home, at);
			if (x.Kind == Behaviour.Worm && x.Critter != null && d * d < best)
			{
				(a, best) = (x, d * d);
			}
		}
		if (a == null)
		{
			return;
		}
		Critter c = a.Critter!;
		c.Landed = false;
		if (slam)
		{
			c.Mode = Mode.Sink;
			c.Timer2 = -1.0f;
			PlayClip(a, c.SinkClip);
			TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Model.Position);
		}
		else
		{
			PlayClip(a, c.SquashClip);
			c.Timer = 1.7f;
			c.Timer2 = 0.0f;
		}
	}

	// MOVE S6/S7: a005 and up 2.5 m at 10 m/s out of the (new) hole; START takes over from there.
	private static void Emerge(Actor a, Critter c)
	{
		c.Mode = Mode.Up;
		c.Timer = 0.0f;
		c.AfterMove = true;
		if (c.PopClip >= 0)
		{
			Animation.CrossFade(a.Model, c.PopClip, 0.2f);
		}
		TwinsanityAudio.Creature(TwinsanityAudio.Call.WormPop, a.Model.Position);
	}

	private void UpdateMonkey(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		Vector3 p = a.Model.Position;
		float d = Horizontal(p, crashPos);
		UpdateFruit(a, c, dt, crashPos);
		switch (c.Mode)
		{
			case Mode.Idle:
				PlayClip(a, a.IdleClip);
				c.Timer -= dt;
				if (d < MonkeyAggro && d > MonkeyMinRange && !c.FruitFlying && TryClaimTree(c, p, out Vector3 tree))
				{
					c.Tree = tree;
					c.Mode = Mode.GoTree;
				}
				else if (c.Timer <= 0.0f)
				{
					float ang = RandomRange(0.0f, MathF.PI * 2.0f);
					c.Target = a.Home + new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang)) * RandomRange(0.0f, 4.0f);
					c.Mode = Mode.Walk;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.Chatter, a.Model.Position);
				}
				break;
			case Mode.Walk:
				PlayClip(a, c.WalkClip);
				if (WalkTo(a, c.Target, MonkeyWalk, dt, true))
				{
					c.Mode = Mode.Idle;
					c.Timer = RandomRange(1.0f, 5.0f);
				}
				break;
			case Mode.GoTree:
				if (d > MonkeyAggro)
				{
					c.Mode = Mode.Idle;
					break;
				}
				PlayClip(a, c.WalkClip);
				if (WalkTo(a, c.Tree + new Vector3(0.0f, 0.0f, -0.6f), MonkeyWalk, dt, true))
				{
					c.Center = Vector3.Zero; // climb anchor, set when the climb starts
					c.Mode = Mode.ClimbUp;
					PlayClip(a, c.ClimbClip);
				}
				break;
			case Mode.ClimbUp:
				// Slide onto the trunk while rising: WalkTo stops 0.6 m short of it (and Separate
				// pushes them back further), so a straight vertical climb floated in the air beside
				// the trunk. Rig: the monkey hugs the trunk (cmp_monkey.png).
				if (c.Center == Vector3.Zero)
				{
					Vector3 off = p - c.Tree;
					off.Y = 0.0f;
					c.Center = c.Tree + (off.LengthSquared() > 1e-4f ? Vector3.Normalize(off) * 0.45f : new Vector3(0.0f, 0.0f, -0.45f));
				}
				Vector3 hug = c.Center - p;
				hug.Y = 0.0f;
				float hl = hug.Length();
				if (hl > 0.0f)
				{
					hug *= MathF.Min(hl, MonkeyWalk * 0.8f * dt);
				}
				a.Model.Position = p + hug + new Vector3(0.0f, MonkeyClimbUp * dt, 0.0f);
				if (a.Model.Position.Y >= c.Tree.Y + MonkeyTreeTop)
				{
					c.Mode = Mode.Shake;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.TreeShake, a.Model.Position);
					c.Timer = 7.0f; // rig m28: 39.9 s -> 47.0 s in the tree
					PlayClip(a, c.ShakeClip);
				}
				break;
			case Mode.Shake:
				c.Timer -= dt;
				if (c.Timer <= 0.0f)
				{
					// A fruit drops from the tree top to the ground beside the trunk.
					float ang = RandomRange(0.0f, MathF.PI * 2.0f);
					Vector3 at = c.Tree + new Vector3(MathF.Cos(ang), 6.0f, MathF.Sin(ang)) * 1.0f;
					at.Y = c.Tree.Y + 6.0f;
					if (!c.Fruit.IsValid && FruitModel() is string fm)
					{
						c.Fruit = World.Create();
						c.Fruit.Name = "MonkeyFruit";
						c.Fruit.AddTransform();
						c.Fruit.Position = at;
						c.Fruit.LoadModel(fm);
					}
					if (c.Fruit.IsValid)
					{
						c.Fruit.Position = at;
					}
					c.Mode = Mode.ClimbDown;
					PlayClip(a, c.ClimbClip);
				}
				break;
			case Mode.ClimbDown:
			{
				float ground = GroundY(p, c.Tree.Y);
				a.Model.Position = p - new Vector3(0.0f, MonkeyClimbDown * dt, 0.0f);
				if (a.Model.Position.Y <= ground)
				{
					a.Model.Position = new Vector3(p.X, ground, p.Z);
					c.Mode = c.Fruit.IsValid ? Mode.GoFruit : Mode.Idle;
				}
				break;
			}
			case Mode.GoFruit:
				PlayClip(a, c.WalkClip);
				if (c.FruitFlying)
				{
					break;
				}
				if (WalkTo(a, c.Fruit.Position, MonkeyWalk, dt, true))
				{
					c.Mode = Mode.Pickup;
					c.Timer = 0.5f;
					PlayClip(a, c.PickClip);
				}
				break;
			case Mode.Pickup:
				c.Timer -= dt;
				c.Fruit.Position = p + new Vector3(0.0f, 1.5f, 0.0f);
				if (c.Timer <= 0.0f)
				{
					c.Mode = Mode.Throw;
					TwinsanityAudio.Creature(TwinsanityAudio.Call.Throw, a.Model.Position);
					c.Timer = 0.4f;
					PlayClip(a, c.ThrowClip);
					FaceMovement(a, crashPos - p);
				}
				break;
			case Mode.Throw:
				// No throws past range: once Crash leaves its range the monkey will not release the
				// fruit; it drops the idea and goes idle.
				if (d > MonkeyAggro)
				{
					DropFruit(c);
					c.Mode = Mode.Idle;
					c.Timer = RandomRange(2.0f, 8.0f);
					break;
				}
				c.Timer -= dt;
				c.Fruit.Position = p + new Vector3(0.0f, 1.7f, 0.0f);
				if (c.Timer <= 0.0f)
				{
					// Ballistic at Crash: 20 m/s across, gravity 42 (rig arc: 14 m in 0.7 s,
					// apex ~1.9 m above the hand).
					Vector3 from = c.Fruit.Position, to = crashPos + new Vector3(0.0f, 0.8f, 0.0f);
					float t = MathF.Max(0.2f, Horizontal(from, to) / FruitAcross);
					Vector3 v = (to - from) / t;
					v.Y = (to.Y - from.Y + 0.5f * FruitGravity * t * t) / t;
					c.FruitVelocity = v;
					c.FruitFlying = true;
					c.FruitLife = 0.0f;
					c.Mode = Mode.Idle;
					c.Timer = RandomRange(2.0f, 5.0f);
				}
				break;
		}
	}

	// The fruit a monkey knocks down or throws. FruitLife < 0: dropped from the tree (no damage).
	private void UpdateFruit(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		if (!c.FruitFlying || !c.Fruit.IsValid)
		{
			return;
		}
		c.FruitVelocity.Y -= FruitGravity * dt;
		Vector3 p = c.Fruit.Position + c.FruitVelocity * dt;
		c.Fruit.Position = p;
		bool thrown = c.FruitLife >= 0.0f;
		if (thrown && Vector3.Distance(p, crashPos + new Vector3(0.0f, 0.8f, 0.0f)) < 1.0f)
		{
			_host?.DamagePlayer(p, DeathKind.Generic);
			DropFruit(c);
			return;
		}
		float ground = GroundY(p, c.Tree.Y);
		if (p.Y <= ground)
		{
			// Rig (track_monkey.csv f1): the fruit bounces (restitution ~1/3) and then rolls away,
			// 3.0 -> 1.6 m/s over ~4 s and ~15 m, then despawns.
			p = new Vector3(p.X, ground, p.Z);
			c.Fruit.Position = p;
			if (!c.Landed)
			{
				c.Landed = true;
				c.FruitVelocity.Y = MathF.Max(0.0f, -c.FruitVelocity.Y / 3.0f);
				// Rig: after landing the fruit rolls at 3.0 -> 1.6 m/s, never at throw speed.
				float sp = new Vector2(c.FruitVelocity.X, c.FruitVelocity.Z).Length();
				if (sp > 3.0f)
				{
					c.FruitVelocity.X *= 3.0f / sp;
					c.FruitVelocity.Z *= 3.0f / sp;
				}
				c.FruitFlying = c.FruitVelocity.Y > 0.5f;
				if (!c.FruitFlying && thrown)
				{
					DropFruit(c);
				}
			}
			else
			{
				c.FruitVelocity.Y = 0.0f;
				float sp = new Vector2(c.FruitVelocity.X, c.FruitVelocity.Z).Length();
				float ns = MathF.Max(0.0f, sp - 0.35f * dt);
				c.FruitVelocity.X *= ns / MathF.Max(sp, 0.001f);
				c.FruitVelocity.Z *= ns / MathF.Max(sp, 0.001f);
				c.FruitLife += dt;
				if (c.FruitLife > 4.0f || (thrown && ns <= 0.1f))
				{
					DropFruit(c);
				}
			}
		}
		{
		}
	}

	private static void DropFruit(Critter c)
	{
		{
		}
		c.FruitFlying = false;
	}

	private static string? FruitModel()
	{
		const string path = "project://assets/models/objects/MOBILEWUMPA/MOBILEWUMPA.gltf";
		return Assets.ReadText(path) != null ? path : null;
	}

	// Nearest tree no other monkey is heading to or climbing. Several monkeys sharing one trunk
	// pile up at its base (Separate keeps them apart, so none ever arrives) and stall the cycle.
	// ponytail: one monkey per tree is inferred, not rig-measured; if the rig shows trunk sharing,
	// stack the climbers instead.
	private bool TryClaimTree(Critter self, Vector3 p, out Vector3 tree)
	{
		tree = default;
		float best = float.MaxValue;
		foreach (Vector3 t in _monkeyTrees)
		{
			bool taken = false;
			foreach (Actor o in _actors)
			{
				Critter? oc = o.Critter;
				if (oc != null && oc != self && o.Alive && oc.Tree == t
					&& oc.Mode is Mode.GoTree or Mode.ClimbUp or Mode.Shake or Mode.ClimbDown)
				{
					taken = true;
					break;
				}
			}
			float h = Horizontal(t, p);
			if (!taken && h < best)
			{
				best = h;
				tree = t;
			}
		}
		return best < float.MaxValue;
	}
}
