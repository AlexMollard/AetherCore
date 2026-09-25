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
///   80 m (script 6400; counter 40 in steps of 8); a crab notices Crash inside 10 m, waits
///   3.5 s and then charges at 2.7 m/s (rig) - touching him hurts.
/// - Skunk: the generic COM_CREATURE_BASIC charger: angry inside 11.2 m, charges, melees
///   inside 4 m, gives up beyond 17.3 m.
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
	// Bird clump (rig: track_b0.csv k1-k3).
	private const float FlockSpeed = 19.7f;
	// Chicken (script COM_GLOBAL_CHICKEN_DEFAULT; rig track_long.csv).
	private const float ChickenPanic = 5.0f;
	private const float ChickenCalm = 7.0f;
	private const float ChickenWalk = 1.0f;
	// ponytail: the flee speed was not captured on the rig (the chicken was off screen); 3 m/s
	// is the old tuned value. Upgrade path: track a chicken while Crash runs at it.
	private const float ChickenFlee = 3.0f;
	// Crab (rig: track_crab2.csv, crab2_wander.csv).
	private const float CrabWanderRadius = 3.5f;
	private const float CrabNotice = 10.0f;
	private const float CrabNoticeDelay = 3.5f;
	private const float CrabSpeed = 2.7f;
	private const float CrabGiveUp = 20.0f;
	// Worm (script COM_EARTH_WORM_START, rig track_worm.csv).
	private const float WormPopRadius = 14.14f;
	private const float WormHideRadius = 13.42f;
	private const float WormDepth = 2.5f;
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

	private enum Mode { Idle, Walk, Flee, Climb, Circle, Land, Rest, Charge, Up, Down, GoTree, ClimbUp, Shake, ClimbDown, GoFruit, Pickup, Throw }

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
		public float Notice, NoticeDelay, GiveUp;
		public float Timer2;
		public int FlyClip = -1, ClimbClip = -1, WalkClip = -1, RestClip = -1, PopClip = -1, SinkClip = -1, SquashClip = -1, SpunClip = -1, ThrowClip = -1, PickClip = -1, ShakeClip = -1;
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

	// Creature spawners and the ecology manager have no model: they spawn copies of the instance
	// they link. True when this instance is one (it is then consumed).
	private bool TryRegisterSpawner(string objectName, JsonElement instance, Matrix4x4 transform, Vector3 position)
	{
		string n = NameKey(objectName);
		bool coop = n.StartsWith("act_creature_spawner");
		bool ecology = n.StartsWith("act_util_ecology_manager");
		if ((!coop && !ecology) || instance.ValueKind != JsonValueKind.Object || !instance.TryGetProperty("links", out JsonElement links))
		{
			return false;
		}
		var s = new Spawner
		{
			Ecology = ecology,
			TemplateKey = ChunkKey(transform, instance, links[0].GetInt32()),
			Position = position,
			Points = PointList(instance, "points", transform),
		};
		// Coop: the instance's own count (params[2], 4 at the beach coop - 4 chickens on the rig).
		// Ecology: COM_UTIL_ECOLOGY_MANAGER_DEFAULT sets counter 40 and spends 8 per spawn.
		s.Count = ecology ? 5 : (instance.TryGetProperty("params", out JsonElement pr) && pr.GetArrayLength() > 2 ? pr[2].GetInt32() : 1);
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
				c.WalkClip = Animation.Find(e, "a002");
				c.Mode = Mode.Idle;
				c.Timer = RandomRange(1.0f, 10.0f);
				break;
			case Behaviour.Crab:
				c.WalkClip = Animation.Find(e, "a002");
				c.Notice = CrabNotice;
				c.NoticeDelay = CrabNoticeDelay;
				c.GiveUp = CrabGiveUp;
				c.Mode = Mode.Idle;
				break;
			case Behaviour.Skunk:
				// COM_CREATURE_BASIC_DEFAULT turns angry inside 11.2 m (125); BASIC_ANGRY_CHARGE
				// gives up beyond 17.3 m (300) and melees (a021) inside 4 m (16).
				c.WalkClip = Animation.Find(e, "a002");
				c.ThrowClip = Animation.Find(e, "a021");
				c.Notice = 11.18f;
				c.GiveUp = 17.32f;
				c.Mode = Mode.Idle;
				break;
			case Behaviour.Worm:
				// COM_EARTH_WORM_START: a006 pops up, a005 sinks; a012 squash-launch, a011 spun.
				c.PopClip = Animation.Find(e, "a006");
				c.SinkClip = Animation.Find(e, "a005");
				c.SquashClip = Animation.Find(e, "a012");
				c.SpunClip = Animation.Find(e, "a011");
				c.Mode = Mode.Down;
				a.Model.Position = a.Home - new Vector3(0.0f, WormDepth, 0.0f);
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
			if (!s.Ecology)
			{
				for (int i = 0; i < s.Count; i++)
				{
					Vector3 at = s.Points.Count > 0 ? s.Points[i % s.Points.Count] : s.Position;
					Actor? c = SpawnCopy(info, at);
					if (c?.Critter != null)
					{
						c.Critter.Points = s.Points;
						s.Live.Add(c);
					}
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
	// half the overlap per side, horizontally only.
	private void Separate()
	{
		for (int i = 0; i < _actors.Count; i++)
		{
			Actor? x = _actors[i];
			if (!x.Alive || x.Critter == null || x.DeathTimer >= 0.0f)
			{
				continue;
			}
			for (int j = i + 1; j < _actors.Count; j++)
			{
				Actor? y = _actors[j];
				if (!y.Alive || y.Critter == null || y.DeathTimer >= 0.0f)
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
			if (!a.Alive || a.Critter == null || a.DeathTimer >= 0.0f)
			{
				continue;
			}
			Vector3 p = a.Model.Position;
			if (new Vector2(center.X - p.X, center.Z - p.Z).Length() < radius && MathF.Abs(center.Y - p.Y) < radius)
			{
				PlayClip(a, a.DeathClip >= 0 ? a.DeathClip : a.MoveClip);
				SetLooping(a.Model, false);
				a.DeathTimer = 1.0f;
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
		if (c?.Critter != null)
		{
			// Inside the coop: the ground ray from above would land on its roof.
			c.Model.Position = s.Position;
			c.Home = s.Position;
			c.Critter.Points = s.Points;
			s.Live.Add(c);
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
				// walked toward left at 15.5 m - it takes a moving Crash to scare them.
				Vector3 cv = _player?.Velocity ?? Vector3.Zero;
				c.Timer2 -= dt;
				if ((Horizontal(p, crashPos) < GullTakeOffRadius && cv.X * cv.X + cv.Z * cv.Z > 0.25f) || c.Timer2 <= 0.0f)
				{
					float ang = RandomRange(0.0f, MathF.PI * 2.0f);
					c.Target = new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang));
					c.Mode = Mode.Climb;
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
			PlayClip(a, c.WalkClip);
			WalkTo(a, p + away * 2.0f, ChickenFlee, dt, true);
			return;
		}
		if (c.Mode == Mode.Idle)
		{
			PlayClip(a, a.IdleClip);
			c.Timer -= dt;
			if (c.Timer <= 0.0f)
			{
				// Rig: ~1.3 s walks at 1 m/s between the coop's AI points, ~10 s apart.
				Vector3 at = c.Points.Count > 0 ? c.Points[_rng.Next(c.Points.Count)] : a.Home;
				c.Target = at + new Vector3(RandomRange(-1.0f, 1.0f), 0.0f, RandomRange(-1.0f, 1.0f));
				c.Mode = Mode.Walk;
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

	private void UpdateCrab(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		Vector3 p = a.Model.Position;
		float d = Horizontal(p, crashPos);
		switch (c.Mode)
		{
			case Mode.Idle:
				PlayClip(a, a.IdleClip);
				c.Timer = d < c.Notice ? c.Timer + dt : 0.0f;
				if (c.Timer >= c.NoticeDelay)
				{
					c.Mode = Mode.Charge;
					break;
				}
				// Rig (crab2_wander.csv): with Crash out of range the crabs do not stand still -
				// each strays 2.5-3.6 m from its spawn in slow bursts (displacement over 30 s).
				c.Timer2 -= dt;
				if (c.Timer2 <= 0.0f)
				{
					float ang = RandomRange(0.0f, MathF.PI * 2.0f);
					c.Target = a.Home + new Vector3(MathF.Cos(ang), 0.0f, MathF.Sin(ang)) * RandomRange(0.5f, CrabWanderRadius);
					c.Mode = Mode.Walk;
					c.Timer2 = RandomRange(4.0f, 9.0f);
				}
				break;
			case Mode.Charge:
				if (d > c.GiveUp)
				{
					c.Mode = Mode.Walk;
					break;
				}
				// ponytail: the skunk's charge speed was not captured (it lives 120 m from the rig's
				// start); it shares the crab's measured 2.7 m/s. Upgrade path: track one on the rig.
				PlayClip(a, c.ThrowClip >= 0 && d < 4.0f ? c.ThrowClip : c.WalkClip);
				if (d > 0.6f)
				{
					WalkTo(a, crashPos, CrabSpeed, dt, true);
				}
				TouchCrash(a, crashPos);
				break;
			case Mode.Walk:
				// ponytail: the wander burst speed was not captured (the rig's beach crabs spawned
				// at the shoreline and the long take was lost); 1.2 m/s, half the measured charge.
				PlayClip(a, c.WalkClip);
				if (WalkTo(a, c.Target, CrabSpeed * 0.45f, dt, true))
				{
					c.Mode = Mode.Idle;
					c.Timer = 0.0f;
					c.Timer2 = RandomRange(2.0f, 6.0f);
				}
				break;
		}
	}

	// Contact with Crash: his attacks kill the creature, otherwise it hurts him.
	private void TouchCrash(Actor a, Vector3 crashPos)
	{
		CrashPlayer? player = _player;
		Vector3 p = a.Model.Position;
		if (player == null || Horizontal(p, crashPos) >= EnemyHitRadius || crashPos.Y >= p.Y + 1.6f || crashPos.Y + 1.8f <= p.Y)
		{
			return;
		}
		bool attacked = player.IsSpinning || player.IsSliding || player.IsSlamming
			|| (player.Velocity.Y < -2.0f && crashPos.Y > p.Y + 0.8f);
		if (attacked)
		{
			PlayClip(a, a.DeathClip >= 0 ? a.DeathClip : a.MoveClip);
			SetLooping(a.Model, false);
			a.DeathTimer = 1.0f;
		}
		else
		{
			_host?.DamagePlayer(p, DeathKind.Generic);
		}
	}

	private void UpdateWorm(Actor a, Critter c, float dt, Vector3 crashPos)
	{
		float d = Vector3.Distance(a.Home, crashPos);
		Vector3 down = a.Home - new Vector3(0.0f, WormDepth, 0.0f);
		Vector3 p = a.Model.Position;
		switch (c.Mode)
		{
			case Mode.Down:
				if (d < WormPopRadius)
				{
					c.Mode = Mode.Up;
					PlayClip(a, c.PopClip);
				}
				// Rig: 3.68 -> 6.18 in 0.25 s up, 0.2 s down.
				a.Model.Position = new Vector3(p.X, MathF.Max(down.Y, p.Y - WormDepth / 0.2f * dt), p.Z);
				break;
			case Mode.Up:
				a.Model.Position = new Vector3(p.X, MathF.Min(a.Home.Y, p.Y + WormDepth / 0.25f * dt), p.Z);
				if (a.Model.Position.Y >= a.Home.Y && Animation.CurrentClip(a.Model) == c.PopClip && c.Timer <= 0.0f)
				{
					PlayClip(a, a.IdleClip);
				}
				c.Timer = MathF.Max(0.0f, c.Timer - dt);
				if (d > WormHideRadius)
				{
					c.Mode = Mode.Down;
					PlayClip(a, c.SinkClip);
					break;
				}
				CrashPlayer? player = _player;
				if (player != null && a.Model.Position.Y >= a.Home.Y - 0.1f)
				{
					float h = Horizontal(a.Home, crashPos);
					if (h < 0.8f && crashPos.Y > a.Home.Y + 0.3f && crashPos.Y < a.Home.Y + 1.6f && MechanicsWorm.TryLaunch(player, a.Home))
					{
						PlayClip(a, c.SquashClip);
						c.Timer = 1.7f;
					}
					else if (h < 1.3f && player.IsSpinning && c.Timer <= 0.0f)
					{
						PlayClip(a, c.SpunClip);
						c.Timer = 1.7f;
					}
				}
				break;
		}
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
					c.Mode = Mode.ClimbUp;
					PlayClip(a, c.ClimbClip);
				}
				break;
			case Mode.ClimbUp:
				a.Model.Position = p + new Vector3(0.0f, MonkeyClimbUp * dt, 0.0f);
				if (a.Model.Position.Y >= c.Tree.Y + MonkeyTreeTop)
				{
					c.Mode = Mode.Shake;
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
