using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Spawns and runs every non-crate, non-wumpa hub instance that carries a model: ambient
/// critters (birds, butterflies, bats, chickens, crabs, worms, skunks, monkeys), enemies
/// (tribesmen, piranha plants, the training miniboss), pickups (gem / wumpa variants) and
/// animated props. Pure logic objects (managers, sound spots, cutscene directors, spawners,
/// Crash duplicates, crate kinds LevelGameplay owns) are skipped - see ShouldSkip.
///
/// Behaviours follow the original's object scripts and the PCSX2 rig; the per-creature ones
/// (seagulls, butterflies, bird flocks, coop chickens, crabs, worms, monkeys) live in
/// TwinsanityWildlife.cs with their measurements.
/// </summary>
public sealed partial class TwinsanityActors
{
	public interface ITwinsanityHost
	{
		void DamagePlayer(Vector3 from, DeathKind kind);
		void AddWumpa(int n);
		void CreatureTouch(Vector3 position);
		void CollectGem(int slot);
	}

	private enum Behaviour
	{
		Flyer,     // bats: circle their spawn point above its height (not yet rig-measured)
		Seagull,   // stands on its perch; takes off and circles when Crash comes near
		Butterfly, // straight legs around its spawn, lands to rest now and then
		Flock,     // bird clump: its birds fly the clump's path
		Chicken,   // coop chickens: peck about, bolt from Crash
		Crab,      // notices Crash, charges him
		Skunk,     // turns angry and charges Crash
		Worm,      // fixed pop-up worm
		Piranha,   // rooted flycatcher: watches Crash, bites when he is close
		Monkey,    // throws fruit from its tree at Crash
		Parrot,    // spawned by a parrot spawner: perches on its keys, flies between them
		Enemy,     // chases Crash when close; touch hurts, spin/jump/slam kills it
		Shieldbearer, // shielded tribesman: blocks spins, bashes Crash; a slide knocks the shield off
		Pickup,    // spins; collected on touch
		Prop,      // plays its idle clip in place
	}

	private sealed class Actor
	{
		public Behaviour Kind;
		public Entity Model = default;
		public Vector3 Home;
		public float HomeYaw;
		public int IdleClip = -1;
		public int MoveClip = -1;
		public int DeathClip = -1;
		public bool Alive = true;
		public float DeathTimer = -1.0f;
		public float Angle;      // bat orbit phase
		public Critter? Critter;
		public int Gem = -1;     // gem pickups: their TwinsanityPause gem-track slot
		public uint Subtype;     // the instance subtype (shieldbearers: 0 advances on Crash, 1/2 stand)
		// Shieldbearers: instance flag bit 19 (SoftFlagSet 19). COM_EARTH_TRIBESMAN_SHIELDBEARER_DEFAULT and
		// COM_CREATURE_BASIC_DEFAULT only run their active branch (DEFEND / the bare tribesman's approach)
		// with it set; clear, they park in the idle starter (s5 / s3) and he stands his post.
		public bool Engages;
		public bool Shielded;    // shieldbearers until a slide knocks the shield off
		public Entity[] ShieldParts = Array.Empty<Entity>(); // the carried shield's mesh entities (ReadShield)
		public string? ShieldModel;
		public Vector3 ShieldRest;         // the shield at rest, model space
		public Quaternion ShieldRestRot;
		public float Bash;       // shieldbearers: seconds left of the bash clip
		public bool Shoving;     // bare shieldbearers: carrying Crash away from the post (UpdateBareGuard)
		// Death knockback (Knockback.cs): airborne while Flying, then DeathTimer counts the linger.
		public bool Flying, Bounced;
		public Vector3 FlyVelocity;
		public KnockArc Arc;
		public float Tumble, FlyYaw; // tumble angle (rad) and heading while flying
	}

	private readonly List<Actor> _actors = new();
	private readonly List<Actor> _guards = new(); // every shieldbearer, kept for ResetGuards
	private readonly List<Actor> _pickups = new();
	private ITwinsanityHost? _host;
	private CrashPlayer? _player;

	// ponytail: speeds/radii are not in the extracted data (instance floats encode something
	// else); these are play-tuned approximations of the original scripts. Upgrade path: dump
	// each object's behaviour script (logs/crashanims.ps1) and read the real constants.
	private const float ChaseRadius = 6.0f;
	private const float EnemyHitRadius = 1.2f;
	private const float PickupRadius = 1.3f;

	public bool TrySpawn(int objectId, string objectName, string? modelPath, Vector3 position, Vector3 eulerDegrees, float[] floats, uint subtype, JsonElement instance, Matrix4x4 transform)
	{
		if (TryRegisterSpawner(objectName, instance, transform, position))
		{
			return true;
		}
		if (ShouldSkip(objectName))
		{
			return false;
		}
		string? model = modelPath ?? ModelFor(objectName);
		RememberInstance(instance, transform, objectId, objectName, model, eulerDegrees, floats);
		if (model == null)
		{
			return false;
		}
		string key = NameKey(objectName);
		if ((key.StartsWith("act_wumpa_tree") || key.StartsWith("old_act_wumpa_tree")) && subtype == 20)
		{
			// Subtype 20 is the monkeys' tree (COM_WUMPA_TREE_DEFAULT plays a001 at spawn for it).
			_monkeyTrees.Add(position);
		}
		if (TrySpawnPushable(objectName, model, position, eulerDegrees))
		{
			return true;
		}
		Entity e = World.Create();
		e.Name = objectName + "#" + objectId;
		e.AddTransform();
		e.Position = position;
		e.EulerDegrees = eulerDegrees;
		e.LoadModel(model);
		NameAttachedParts(e, model);
		if (key.StartsWith("act_redwumpa"))
		{
			// Wumpa cast no shadow (as TwinsanityWumpa.SpawnModel): at distance they show as specks.
			for (int i = 0; i < e.ChildCount; i++)
			{
				MeshRenderer.SetCastShadows(e.GetChild(i), false);
			}
		}
		RegisterActor(e, objectId, objectName, model, instance, transform, position, eulerDegrees);
		return true;
	}

	// Everything an actor needs after its model entity exists: clips, behaviour, one-shot or
	// looping animation, critter state. Shared by the play-time spawn (above) and the bake bind
	// (TryBind, whose entity came from the bake with its model already loaded).
	private void RegisterActor(Entity e, int objectId, string objectName, string model, JsonElement instance, Matrix4x4 transform, Vector3 position, Vector3 eulerDegrees)
	{
		Actor a = new()
		{
			Model = e,
			Home = position,
			HomeYaw = eulerDegrees.Y,
			Angle = (objectId % 17) * 0.7f,
			Gem = TwinsanityPause.GemSlot(objectName),
		};
		ReadClips(e, a);

		a.Kind = BehaviourOf(objectName);
		a.Subtype = SubtypeOf(instance);
		if (a.Kind == Behaviour.Shieldbearer)
		{
			// COM_EARTH_TRIBESMAN_SHIELDBEARER_DEFEND's DoAnim slots (clip aNNN is slot NNN): 25 its guard
			// stance, 23 the step at Crash; 21 is ATTACK_MELEE's bash.
			a.Shielded = true;
			a.Engages = instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("flags", out JsonElement fl)
				&& (fl.GetUInt32() >> 19 & 1u) != 0;
			ReadShield(a, model);
			a.IdleClip = Animation.Find(e, "a025");
			a.MoveClip = Animation.Find(e, "a023");
			_guards.Add(a);
		}
		// One-shot props (TwinsanityProps.cs) rest until their cue; everything else loops its idle.
		if (a.Kind != Behaviour.Prop || !SetupOneShot(a, objectName, SubtypeOf(instance), model, instance, transform))
		{
			SetLooping(e, true);
			if (a.IdleClip >= 0)
			{
				Animation.SetClip(e, a.IdleClip);
			}
		}
		_actors.Add(a);
		TrackInstance(instance, transform, a);
		if (a.Kind == Behaviour.Pickup)
		{
			_pickups.Add(a);
		}
		if (a.Kind is Behaviour.Seagull or Behaviour.Butterfly or Behaviour.Flock or Behaviour.Chicken
			or Behaviour.Crab or Behaviour.Skunk or Behaviour.Worm or Behaviour.Monkey or Behaviour.Piranha)
		{
			SetupCritter(a, objectName, instance, transform);
			if (a.Kind == Behaviour.Flock)
			{
				SpawnFlock(a, model);
			}
		}
	}

	// The bake bind: one pre-built entity with its marker role and world-space identity JSON.
	// Spawner roles register the spawner (the marker entity is just their position holder);
	// everything else registers the actor around the existing model entity.
	public bool TryBind(Entity e, int role, JsonElement instance)
	{
		if (role is SpawnerCreature or SpawnerParrot)
		{
			Vector3 at = instance.TryGetProperty("position", out JsonElement p) ? Vec3(p) : e.Position;
			TryRegisterSpawner(ObjectNameOf(instance), instance, Matrix4x4.Identity, at);
			return true;
		}
		int objectId = instance.TryGetProperty("object", out JsonElement ob) ? ob.GetInt32() : 0;
		string objectName = ObjectNameOf(instance);
		string? model = instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : ModelFor(objectName);
		Vector3 position = instance.TryGetProperty("position", out JsonElement pp) ? Vec3(pp) : e.Position;
		Vector3 euler = instance.TryGetProperty("euler", out JsonElement el) ? Vec3(el) : e.EulerDegrees;
		RememberInstance(instance, Matrix4x4.Identity, objectId, objectName, model, euler, FloatsOf(instance));
		string key = NameKey(objectName);
		if ((key.StartsWith("act_wumpa_tree") || key.StartsWith("old_act_wumpa_tree")) && SubtypeOf(instance) == 20)
		{
			_monkeyTrees.Add(position);
		}
		if (TryBindPushable(e, objectName, position, euler, model))
		{
			return true;
		}
		RegisterActor(e, objectId, objectName, model ?? "", instance, Matrix4x4.Identity, position, euler);
		return true;
	}

	private static string ObjectNameOf(JsonElement instance)
		=> InstanceName(instance, instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : null);

	/// <summary>The instance's object name. Layer-5 instances (crates, fruit and the hub's colour gems
	/// GEM_YELLOW/PURPLE/GREEN...) carry none, so they are known by their object's model file
	/// (GEM_YELLOW/GEM_YELLOW.gltf, BASICCRATE/BASICCRATE_0.gltf); "object_N" was never a pickup, which
	/// left those gems as dead props. Shared by the JSON build, the bake and the bake bind.</summary>
	internal static string InstanceName(JsonElement instance, string? model)
	{
		if (instance.TryGetProperty("name", out JsonElement n) && n.GetString() is { Length: > 0 } name)
		{
			return name;
		}
		if (model != null)
		{
			string stem = System.IO.Path.GetFileNameWithoutExtension(model);
			int u = stem.LastIndexOf('_');
			return u > 0 && int.TryParse(stem.AsSpan(u + 1), out _) ? stem[..u] : stem;
		}
		return $"object_{(instance.TryGetProperty("object", out JsonElement o) ? o.GetInt32() : 0)}";
	}

	private static uint SubtypeOf(JsonElement instance)
		=> instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("subtype", out JsonElement st) ? st.GetUInt32() : 0u;

	private static float[] FloatsOf(JsonElement instance)
	{
		if (instance.ValueKind != JsonValueKind.Object || !instance.TryGetProperty("floats", out JsonElement fl) || fl.ValueKind != JsonValueKind.Array)
		{
			return Array.Empty<float>();
		}
		var list = new List<float>();
		foreach (JsonElement f in fl.EnumerateArray())
		{
			list.Add(f.GetSingle());
		}
		return list.ToArray();
	}

	public void Update(float dt, CrashPlayer player, ITwinsanityHost host)
	{
		_host = host;
		_player = player;
		Vector3 crashPos = player.Self.IsValid ? player.Self.Position : Vector3.Zero;

		foreach (Actor a in _actors)
		{
			if (!a.Alive)
			{
				continue;
			}
			if (a.DeathTimer >= 0.0f)
			{
				if (a.Flying)
				{
					Fly(a, dt);
					continue;
				}
				a.DeathTimer -= dt;
				if (a.DeathTimer < 0.0f)
				{
					// The pop: its disc particles and sound, and the body is gone at once.
					bool chicken = a.Kind == Behaviour.Chicken;
					Vector3 at = a.Model.Position + new Vector3(0.0f, chicken ? 0.35f : 0.5f, 0.0f);
					CrateFx.CreaturePop(at, chicken);
					TwinsanityAudio.Creature(chicken ? TwinsanityAudio.Call.ChickenPop : TwinsanityAudio.Call.Pop, at);
					a.Alive = false;
					if (a.Kind == Behaviour.Shieldbearer)
					{
						a.Model.SetActive(false); // ResetGuards brings him back for a zone replay
					}
					else
					{
						a.Model.Destroy();
					}
				}
				continue;
			}
			if (a.Kind is Behaviour.Chicken or Behaviour.Crab or Behaviour.Skunk or Behaviour.Monkey && Attacked(a, crashPos))
			{
				continue; // his attacks kill a creature whatever it is doing
			}
			switch (a.Kind)
			{
				case Behaviour.Flyer:
					UpdateFlyer(a, dt);
					break;
				case Behaviour.Enemy:
					UpdateEnemy(a, dt, crashPos);
					break;
				case Behaviour.Shieldbearer:
					UpdateShieldbearer(a, dt, crashPos);
					break;
				case Behaviour.Pickup:
					UpdatePickup(a, dt, crashPos, host);
					break;
				case Behaviour.Seagull:
					UpdateSeagull(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Butterfly:
					UpdateButterfly(a, a.Critter!, dt);
					break;
				case Behaviour.Flock:
					UpdateFlock(a, a.Critter!, dt);
					break;
				case Behaviour.Chicken:
					UpdateChicken(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Crab:
					UpdateCrab(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Skunk:
					UpdateSkunk(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Piranha:
					UpdatePiranha(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Worm:
					UpdateWorm(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Monkey:
					UpdateMonkey(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Parrot:
					UpdateParrot(a, a.Critter!, dt);
					break;
			}
		}
		UpdateCritters();
		TouchHost();
		Separate();
		UpdateEcology(crashPos, dt);
		UpdateParrotSpawners(crashPos);
		_actors.AddRange(_pending);
		_pending.Clear();
		UpdateDroppedShields(dt);
		UpdateOneShots(dt, crashPos);
		UpdatePushables(dt, player);
		_actors.RemoveAll(a => !a.Alive);
		_pickups.RemoveAll(a => !a.Alive);
	}

	// ---- behaviour update ----

	private void UpdateFlyer(Actor a, float dt)
	{
		// Circle the spawn point, bobbing above (never below) its height so ground-level
		// butterflies do not dip into the sand.
		Vector3 p = a.Model.Position;
		a.Angle += dt * 0.7f;
		Vector3 target = a.Home + new Vector3(MathF.Cos(a.Angle) * 2.5f, 0.25f + MathF.Sin(a.Angle * 2.3f) * 0.25f, MathF.Sin(a.Angle) * 2.5f);
		Vector3 step = target - p;
		float len = step.Length();
		if (len > 0.001f)
		{
			float max = 1.8f * dt;
			if (len > max)
			{
				step *= max / len;
			}
			a.Model.Position = p + step;
			FaceMovement(a, step);
		}
	}

	/// <summary>A scripted hit on a live enemy (a cutscene actor's blow): the nearest standing within 2 of `at` is
	/// knocked away from `from`, with the gen_IMPACT1 flash the rig shows on the shieldbearer (station 3).
	/// The nearest, not the first found: the mouth guard can stand near station 3's guard, and the blow is
	/// the scene's, for station 3's guard alone.
	/// ponytail: this is the enemy's death knockback; the disc's shieldbearer gets up again after its HIT
	/// script's 4 s lie-down, which the rig's station 3 capture does not reach.</summary>
	public void KnockEnemy(Vector3 at, Vector3 from)
	{
		static float Sq(Vector3 p, Vector3 q) => (p.X - q.X) * (p.X - q.X) + (p.Z - q.Z) * (p.Z - q.Z);
		Actor? a = null;
		float best = 4.0f;
		foreach (Actor x in _actors)
		{
			float d = Sq(x.Model.Position, at);
			if (x.Alive && x.DeathTimer < 0.0f && x.Kind is Behaviour.Enemy or Behaviour.Shieldbearer && d < best)
			{
				(a, best) = (x, d);
			}
		}
		if (a != null)
		{
			EndShove(a);
			CrateFx.ImpactFlash(a.Model.Position + new Vector3(0.0f, 0.8f, 0.0f));
			Kill(a, from);
		}
	}

	private void UpdateEnemy(Actor a, float dt, Vector3 crashPos)
	{
		CrashPlayer? player = _player;
		if (player == null)
		{
			return;
		}
		Vector3 p = a.Model.Position;
		float dx = crashPos.X - p.X, dz = crashPos.Z - p.Z;
		float distSq = dx * dx + dz * dz;

		// Tribesmen chase Crash when he is close, but never leave their post by much.
		if (distSq < ChaseRadius * ChaseRadius)
		{
			float dist = MathF.Sqrt(distSq);
			Vector3 dir = dist > 0.001f ? new Vector3(dx / dist, 0.0f, dz / dist) : Vector3.UnitZ;
			Vector3 toHome = a.Home - p;
			toHome.Y = 0.0f;
			bool leashed = toHome.LengthSquared() > 16.0f && Vector3.Dot(dir, Vector3.Normalize(toHome)) < 0.0f;
			if (!leashed && dist > 1.0f)
			{
				Vector3 step = dir * 1.5f * dt;
				a.Model.Position = p + step;
				FaceMovement(a, step);
				PlayClip(a, a.MoveClip >= 0 ? a.MoveClip : a.IdleClip);
			}
		}
		else
		{
			PlayClip(a, a.IdleClip);
		}

		bool horizontal = distSq < EnemyHitRadius * EnemyHitRadius;
		bool vertical = crashPos.Y < p.Y + 1.6f && crashPos.Y + 1.8f > p.Y;
		if (!(horizontal && vertical))
		{
			return;
		}
		// Crash's attacks: spin, slide, slam, or landing on top of it.
		bool attacked = player.IsSpinning || player.IsSliding || player.IsSlamming
			|| (player.Velocity.Y < -2.0f && crashPos.Y > p.Y + 0.8f);
		if (attacked)
		{
			Kill(a, crashPos);
			_host?.AddWumpa(1);
		}
		else
		{
			_host?.DamagePlayer(p, DeathKind.Generic);
		}
	}

	// The shieldbearers (disc COM_EARTH_TRIBESMAN_SHIELDBEARER_DEFEND/_HIT, rig logs/tutorialroute/rig_guard_*):
	// within sqrt(200) of Crash he faces him; subtype 0 steps at him at 2.75/s but never past 6 from his post
	// (MeToInitPosSqrDist 36) and walks back when Crash leaves. His HIT script ignores every attack while his
	// hit points are above 10 except AgentWasSlid: spins, slams and jumps do nothing to him, while touching
	// him any other way gets Crash bashed. A slide knocks the shield off (HP 20 -> 10) and Crash slides on
	// past; he stays up and from then on is a plain tribesman (DEFAULT state 3, UpdateBareGuard) who shoves
	// Crash out of his ground and whom a spin knocks away. All of the stepping, turning and shoving needs
	// instance flag bit 19 (Actor.Engages): huba's guards have it and come at Crash; Hub B's guard 38 does
	// not and stands his post (logs/hubb/rig_w6_sheet.png).
	private const float GuardSightSq = 200.0f;
	private const float GuardSpeed = 2.75f;
	private const float GuardLeash = 6.0f;
	private const float GuardStop = 1.6f; // he halts short of touching: Crash standing by him is not bashed
	// A slide meets the shield he holds out in front, so it reaches him from where he halts; the bare
	// body radius alone let a slide at a guard standing at his leash end stop short or pass him by.
	// ponytail: the rig (rig_g2_leashF_sheet.png) knocks it off from ~1.5 m and a slide ending ~2 m
	// short does not (rig_g2_leashE_sheet.png); the disc's collision radius is not decoded.
	private const float GuardSlideReach = GuardStop;

	private void UpdateShieldbearer(Actor a, float dt, Vector3 crashPos)
	{
		CrashPlayer? player = _player;
		if (player == null)
		{
			return;
		}
		if (!a.Shielded)
		{
			// The slide that took his shield carries Crash on past him: that same slide must not also
			// count as the attack that knocks the bare tribesman away.
			a.Bash -= dt;
			if (a.Bash <= 0.0f)
			{
				UpdateBareGuard(a, player, dt, crashPos);
			}
			return;
		}
		Vector3 p = a.Model.Position;
		float dx = crashPos.X - p.X, dz = crashPos.Z - p.Z;
		float distSq = dx * dx + dz * dz;
		bool sees = a.Engages && distSq < GuardSightSq; // not engaged: no DEFEND, he neither turns nor steps
		Vector3 step = Vector3.Zero;
		if (sees)
		{
			float dist = MathF.Sqrt(distSq);
			Vector3 dir = dist > 0.001f ? new Vector3(dx / dist, 0.0f, dz / dist) : Vector3.UnitZ;
			FaceMovement(a, dir);
			if (a.Subtype == 0 && dist > GuardStop)
			{
				Vector3 next = p + dir * MathF.Min(GuardSpeed * dt, dist - GuardStop);
				Vector3 fromHome = new(next.X - a.Home.X, 0.0f, next.Z - a.Home.Z);
				if (fromHome.Length() > GuardLeash)
				{
					next = new Vector3(a.Home.X, next.Y, a.Home.Z) + Vector3.Normalize(fromHome) * GuardLeash;
				}
				step = next - p;
			}
		}
		else if (a.Engages && a.Subtype == 0)
		{
			Vector3 home = new(a.Home.X - p.X, 0.0f, a.Home.Z - p.Z);
			float len = home.Length();
			if (len > 0.05f)
			{
				step = home * MathF.Min(1.0f, GuardSpeed * dt / len);
				FaceMovement(a, step);
			}
		}
		a.Model.Position = p + step;
		a.Bash = MathF.Max(0.0f, a.Bash - dt);
		if (a.Bash <= 0.0f)
		{
			PlayClip(a, step.LengthSquared() > 1e-8f ? a.MoveClip : a.IdleClip);
		}

		float reach = player.IsSliding ? GuardSlideReach : EnemyHitRadius;
		bool touching = distSq < reach * reach && crashPos.Y < p.Y + 1.6f && crashPos.Y + 1.8f > p.Y;
		if (!touching || player.IsSpinning)
		{
			return; // the shield takes a spin
		}
		if (player.IsSliding)
		{
			a.Shielded = false;
			a.Bash = 0.8f;
			CrateFx.ImpactFlash(p + new Vector3(0.0f, 0.8f, 0.0f));
			DropShield(a, crashPos);
			Log.Info($"[Twinsanity] shieldbearer {a.Model.Name} lost his shield to a slide");
			return;
		}
		a.Bash = 1.0f;
		PlayClip(a, Animation.Find(a.Model, "a021"));
		_host?.DamagePlayer(p, DeathKind.Generic);
	}

	// The bare tribesman (DEFAULT state 3, COM_CREATURE_BASIC_DEFAULT; rig rig_g2_bare_sheet.png,
	// rig_g2_bare_push.png, 0 masks): he never hurts Crash. While Crash is within sqrt(200) of his post
	// he comes at him and, on contact, walks him backwards away from the post (~5 m/s, Crash carried
	// upright with no control) until Crash is sqrt(200) out - the rig's second shove ended 14.3 m from
	// the post - and then stands watching him. A spin, slide, slam or landing on him knocks him away.
	// ponytail: the approach reuses his shielded step speed and the shove is a straight carry at a
	// constant 5 m/s (the rig's two shoves: 8 m in 1.6 s, 9.6 m in 2.4 s); Crash's feet keep their
	// height, which holds on the flat mouth clearing. Upgrade path: time the approach and trace the
	// shove's ground contact on the rig.
	private const float ShoveSpeed = 5.0f;

	private void UpdateBareGuard(Actor a, CrashPlayer player, float dt, Vector3 crashPos)
	{
		Vector3 p = a.Model.Position;
		float dx = crashPos.X - p.X, dz = crashPos.Z - p.Z;
		float distSq = dx * dx + dz * dz;
		bool near = distSq < EnemyHitRadius * EnemyHitRadius && crashPos.Y < p.Y + 1.6f && crashPos.Y + 1.8f > p.Y;
		bool attacked = player.IsSpinning || player.IsSliding || player.IsSlamming
			|| (player.Velocity.Y < -2.0f && crashPos.Y > p.Y + 0.8f);
		if (near && attacked && !a.Shoving)
		{
			Kill(a, crashPos);
			_host?.AddWumpa(1);
			return;
		}
		if (!a.Engages)
		{
			PlayClip(a, a.IdleClip); // CREATURE_BASIC_DEFAULT s2 -> s3 without flag 19: he stands, spinnable
			return;
		}
		Vector3 fromPost = new(crashPos.X - a.Home.X, 0.0f, crashPos.Z - a.Home.Z);
		bool inside = fromPost.LengthSquared() < GuardSightSq;
		if (a.Shoving)
		{
			if (!inside || player.IsDead)
			{
				EndShove(a);
				return;
			}
			Vector3 dir = Vector3.Normalize(fromPost);
			Vector3 step = dir * ShoveSpeed * dt;
			player.RideFeet = crashPos + step;
			a.Model.Position = p + step;
			FaceMovement(a, -dir);
			PlayClip(a, a.MoveClip);
			return;
		}
		float dist = MathF.Sqrt(distSq);
		Vector3 toCrash = dist > 0.001f ? new Vector3(dx / dist, 0.0f, dz / dist) : Vector3.UnitZ;
		if (!inside || player.IsDead || player.RideFeet != null)
		{
			FaceMovement(a, toCrash);
			PlayClip(a, a.IdleClip);
			return;
		}
		if (near)
		{
			a.Shoving = true;
			return;
		}
		Vector3 walk = toCrash * MathF.Min(GuardSpeed * dt, dist);
		a.Model.Position = p + walk;
		FaceMovement(a, walk);
		PlayClip(a, a.MoveClip);
	}

	private void EndShove(Actor a)
	{
		if (a.Shoving && _player != null)
		{
			_player.RideFeet = null;
		}
		a.Shoving = false;
	}

	/// <summary>Crash respawned at a zone checkpoint (TwinsanityCutscenes.Respawned): the zone's scenes replay,
	/// and station 3's needs its guard back for Coco to knock down again, as the rig's replay shows. Every
	/// shieldbearer goes back to how the level placed him: at his post, shield on, whatever happened to him.
	/// ponytail: the rig shows only station 3's guard back; the mouth guard is reset the same way.</summary>
	public void ResetGuards()
	{
		int restored = 0;
		foreach (Actor a in _guards)
		{
			restored += !a.Alive || a.DeathTimer >= 0.0f || !a.Shielded ? 1 : 0;
			EndShove(a);
			a.Alive = true;
			a.DeathTimer = -1.0f;
			a.Flying = a.Bounced = false;
			a.Shielded = true;
			a.Bash = 0.0f;
			a.Model.SetActive(true);
			a.Model.Position = a.Home;
			a.Model.EulerDegrees = new Vector3(0.0f, a.HomeYaw, 0.0f);
			foreach (Entity part in a.ShieldParts)
			{
				MeshRenderer.SetVisible(part, true);
			}
			SetLooping(a.Model, true);
			PlayClip(a, a.IdleClip);
			if (!_actors.Contains(a))
			{
				_actors.Add(a);
			}
		}
		Log.Info($"[Twinsanity] {_guards.Count} shieldbearers back at their posts, {restored} of them restored");
	}

	// The shield (WEAPON_NATIVE_SHIELD, attached on exit point 2 by his INIT script) is baked onto his
	// arm by tw-extract as the model's last mesh entities; <model>.attach.json says how many. They are
	// last only straight after LoadModel (a baked prefab's instances do not keep the child order), so the
	// spawn (NameAttachedParts, here and in TwinsanityBake) names them and ReadShield finds them by name.
	private const string AttachedPartName = " attached";

	private static JsonDocument? Attachments(string? model)
	{
		string? text = model != null && model.EndsWith(".gltf", StringComparison.Ordinal) ? Assets.ReadText(model[..^5] + ".attach.json") : null;
		return text != null ? JsonDocument.Parse(text) : null;
	}

	internal static void NameAttachedParts(Entity e, string? model)
	{
		using JsonDocument? doc = Attachments(model);
		if (doc == null)
		{
			return;
		}
		int n = 0;
		foreach (JsonElement at in doc.RootElement.GetProperty("attach").EnumerateArray())
		{
			n += at.GetProperty("prims").GetInt32();
		}
		for (int i = Math.Max(0, e.ChildCount - n); i < e.ChildCount; i++)
		{
			e.GetChild(i).Name += AttachedPartName;
		}
	}

	private static void ReadShield(Actor a, string model)
	{
		using JsonDocument? doc = Attachments(model);
		if (doc == null || doc.RootElement.GetProperty("attach").GetArrayLength() == 0)
		{
			return;
		}
		JsonElement at = doc.RootElement.GetProperty("attach")[0]; // his one attachment, the shield
		var parts = new List<Entity>();
		for (int i = 0; i < a.Model.ChildCount; i++)
		{
			if (a.Model.GetChild(i).Name.EndsWith(AttachedPartName, StringComparison.Ordinal))
			{
				parts.Add(a.Model.GetChild(i));
			}
		}
		a.ShieldParts = parts.ToArray();
		a.ShieldModel = at.GetProperty("model").GetString();
		a.ShieldRest = Vec3(at.GetProperty("position"));
		JsonElement r = at.GetProperty("rotation");
		a.ShieldRestRot = new Quaternion(r[0].GetSingle(), r[1].GetSingle(), r[2].GetSingle(), r[3].GetSingle());
	}

	// COM_WEAPON_NATIVE_SHIELD_DROPPED: RequestDetach, the shield falls as a rigid body launched up
	// (Cmd193 ... 10, -6), and 2 s later (TimeInUnit 2) it pops in particles and is destroyed. The rig
	// (rig_guard_slidespin_sheet.png) shows it flung up over his head and gone before the next spin.
	// ponytail: a knockback arc, not a rigid body: 3 m/s away from Crash, 10 m/s up under 30 m/s^2
	// (SetContactRigid's 30), one half bounce. Upgrade path: time its flight on the rig frame by frame.
	private static readonly KnockArc ShieldArc = new(3.0f, 10.0f, 30.0f, 0.5f, 0.5f, 12.0f, 0.0f, 0.0f);
	private const float ShieldLife = 2.0f;

	private sealed class DroppedShield
	{
		public Entity Body;
		public Vector3 Velocity;
		public bool Bounced;
		public float Life = ShieldLife, Tumble, Yaw;
	}

	private readonly List<DroppedShield> _drops = new();

	private void DropShield(Actor a, Vector3 from)
	{
		foreach (Entity part in a.ShieldParts)
		{
			MeshRenderer.SetVisible(part, false);
		}
		Log.Info($"[Twinsanity] shield off {a.Model.Name}: {a.ShieldParts.Length} part(s) hidden, dropping {a.ShieldModel ?? "nothing"}");
		if (a.ShieldModel == null)
		{
			return;
		}
		Matrix4x4 model = TwinsanityLevel.SysRotation(a.Model.EulerDegrees);
		Vector3 at = a.Model.Position + Vector3.Transform(a.ShieldRest, model);
		Entity s = World.Create();
		s.Name = "WEAPON_NATIVE_SHIELD (dropped)";
		s.AddTransform();
		s.Position = at;
		s.EulerDegrees = TwinsanityLevel.EulerOf(Matrix4x4.CreateFromQuaternion(a.ShieldRestRot) * model);
		s.LoadModel(a.ShieldModel);
		_drops.Add(new DroppedShield
		{
			Body = s,
			Velocity = Knockback.Launch(from, at, ShieldArc, Vector3.Transform(Vector3.UnitZ, model)),
			Yaw = s.EulerDegrees.Y,
		});
	}

	private void UpdateDroppedShields(float dt)
	{
		foreach (DroppedShield d in _drops)
		{
			Vector3 p = d.Body.Position, v = d.Velocity;
			bool bounced = d.Bounced;
			if (!Knockback.Step(ref p, ref v, ref bounced, ShieldArc, GroundY(p, p.Y - 40.0f, 0.3f), dt))
			{
				d.Tumble += ShieldArc.Tumble * dt;
				d.Body.EulerDegrees = new Vector3(d.Tumble * (180.0f / MathF.PI), d.Yaw, 0.0f);
			}
			d.Body.Position = p;
			d.Velocity = v;
			d.Bounced = bounced;
			d.Life -= dt;
			if (d.Life <= 0.0f)
			{
				CrateFx.CreaturePop(p + new Vector3(0.0f, 0.3f, 0.0f), false);
				d.Body.Destroy();
			}
		}
		_drops.RemoveAll(d => d.Life <= 0.0f);
	}

	private void UpdatePickup(Actor a, float dt, Vector3 crashPos, ITwinsanityHost host)
	{
		a.Model.EulerDegrees = new Vector3(0.0f, a.Model.EulerDegrees.Y + 120.0f * dt, 0.0f);
		Vector3 center = crashPos + new Vector3(0.0f, 0.9f, 0.0f);
		if (Vector3.DistanceSquared(center, a.Model.Position + new Vector3(0.0f, 0.5f, 0.0f)) < PickupRadius * PickupRadius)
		{
			a.Alive = false;
			a.Model.Destroy();
			if (a.Gem >= 0)
			{
				host.CollectGem(a.Gem);
			}
			else
			{
				host.AddWumpa(1);
			}
		}
	}

	// ---- helpers ----

	private void PlayClip(Actor a, int clip)
	{
		if (clip >= 0 && Animation.CurrentClip(a.Model) != clip)
		{
			Animation.CrossFade(a.Model, clip, 0.2f);
		}
	}

	private void FaceMovement(Actor a, Vector3 step)
	{
		// Models face +Z; the engine yaw rotates it onto the movement direction.
		float yaw = MathF.Atan2(step.X, step.Z) * 180.0f / MathF.PI;
		Vector3 e = a.Model.EulerDegrees;
		a.Model.EulerDegrees = new Vector3(e.X, yaw, e.Z);
	}

	private static void ReadClips(Entity e, Actor a)
	{
		var names = new List<string>();
		for (int i = 0; i < Animation.ClipCount(e); i++)
		{
			string n = Animation.ClipName(e, i);
			if (n.Length > 0)
			{
				names.Add(n);
			}
		}
		names.Sort(StringComparer.Ordinal);
		if (names.Count > 0)
		{
			a.IdleClip = Animation.Find(e, names[0]);
		}
		if (names.Count > 1)
		{
			a.MoveClip = Animation.Find(e, names[1]);
		}
		// Original hurt clips from the behaviour scripts (states.json OnGettingSpinAttacked
		// etc.), where the object has one.
		foreach (string hurt in new[] { "a012", "a001", names.Count > 1 ? names[1] : "" })
		{
			if (hurt.Length > 0)
			{
				int idx = Animation.Find(e, hurt);
				if (idx >= 0)
				{
					a.DeathClip = idx;
					break;
				}
			}
		}
	}

	private static void SetLooping(Entity entity, bool loop)
	{
		ComponentAccess skinned = entity.Component("Skinned Mesh");
		if (skinned.Exists)
		{
			skinned.SetBool("looping", loop);
		}
		for (int i = 0; i < entity.ChildCount; i++)
		{
			SetLooping(entity.GetChild(i), loop);
		}
	}

	private static Behaviour BehaviourOf(string name)
	{
		string n = NameKey(name);
		if (n.StartsWith("act_global_seagull"))
		{
			return Behaviour.Seagull;
		}
		if (n.StartsWith("act_global_butterfly"))
		{
			return Behaviour.Butterfly;
		}
		if (n.StartsWith("act_birdclump"))
		{
			return Behaviour.Flock;
		}
		if (n.StartsWith("act_global_bat"))
		{
			return Behaviour.Flyer;
		}
		if (n.StartsWith("act_global_chicken"))
		{
			return Behaviour.Chicken;
		}
		if (n.StartsWith("act_global_crab") || n.StartsWith("old_act_global_crab"))
		{
			return Behaviour.Crab;
		}
		if (n.StartsWith("act_earth_worm") || n.StartsWith("act_whackaworm_worm"))
		{
			return Behaviour.Worm;
		}
		if (n.StartsWith("act_global_monkey"))
		{
			return Behaviour.Monkey;
		}
		if (n.StartsWith("act_piranhaplant"))
		{
			return Behaviour.Piranha;
		}
		if (n.StartsWith("act_earth_tribesman_shieldbearer"))
		{
			return Behaviour.Shieldbearer;
		}
		if (n.StartsWith("act_earth_tribesman") || n.StartsWith("act_cortex_training_miniboss") || n.StartsWith("act_cortex_creature"))
		{
			return Behaviour.Enemy;
		}
		if (n.StartsWith("act_redwumpa") || n.StartsWith("gem") || n.StartsWith("act_gem") || n.StartsWith("act_power_crystal"))
		{
			return Behaviour.Pickup;
		}
		if (n.StartsWith("act_global_skunk"))
		{
			return Behaviour.Skunk;
		}
		return Behaviour.Prop;
	}

	// Pure logic / audio / cutscene objects, and everything LevelGameplay or CrashPlayer
	// already owns. Listed with the reason.
	private static bool ShouldSkip(string name)
	{
		string raw = name.ToLowerInvariant();
		string n = NameKey(name);
		if (n.StartsWith("act_crash")) return true;                  // Crash duplicates (player spawn markers)
		if (n.StartsWith("redwumpa")) return true;                  // wumpa fruit - object 1, LevelGameplay handles
		if (n.Contains("crate") || n.Contains("akuakucrate") || n.Contains("checkpoint")) return true; // crate kinds: LevelGameplay's rules
		// Script evidence: these are NOT in the hub in free roam - the RM2 groups them into
		// "|Bossarea|Earth_300404_1430_BossActorsOnly_...|e3earthhub_mechoonly_cutsceneonly_...
		// |g_Cortex_BossFight_Actors|" (miniboss + FAKE_* illusions + arena pieces),
		// "|Bossarea|Cutscene|" (assistant), "|HubD|Farmer_Cutscene_Resources_HubD|" (Cortex1,
		// EMU_FARMER), "|Beach|CutsceneResources_Beach|BeachTrainingCutscene|" (CUTSCENE_EXTRA)
		// and "|Beach|FrontEndCutscene|" (DUMMY_FRONTEND_CHARACTER). Cutscene/boss directors
		// enable them; spawn nothing.
		if (n.StartsWith("act_cortex_training_fake") || n.StartsWith("act_cortex_training_miniboss")
			|| n.StartsWith("act_training_miniboss") || n.StartsWith("act_cortex_training_cutscene_assistant")
			|| n.StartsWith("act_cortex_creature") || n.StartsWith("act_cortex_hoverboard")
			|| raw.StartsWith("act_cortex1") || raw.StartsWith("act_cortex2") || raw.StartsWith("act_cortex3")
			|| n.StartsWith("act_cutscene_extra") || n.StartsWith("act_dummy_frontend")
			|| n.StartsWith("act_earth_emu_farmer"))
		{
			return true;
		}
		// Rig (logs/wildlife/orig_coco.png): the COCO_CREATUREs and the training skunk sit in the
		// "|HubA|HubACutscenes|" group and are not in the world in free roam - Crash stood a
		// metre from act_COCO_CREATURE3's spot and nothing was there.
		if (n.StartsWith("act_coco_creature") || n.StartsWith("act_training_cutscene_skunk"))
		{
			return true;
		}
		// The nine huba bats (act_GLOBAL_BAT_DARKPURPLE8) are in "|HubA|HubACutscenes|" too: on the
		// rig they never leave their spots inside the totem entrance (logs/wildlife/bat_rig.csv,
		// Crash 15.6 m away) and none shows in its mouth (bat_rig_3.png). The hubb ones
		// (DARKPURPLE10, "|HubB|HubB_Actors|") are live.
		if (raw.StartsWith("act_global_bat_darkpurple8"))
		{
			return true;
		}
		if (n.Contains("manager") || n.Contains("director") || n.Contains("sound") || n.Contains("_dj")
			|| n.Contains("textmaster") || n.Contains("spawner") || n.Contains("controller")
			|| n.Contains("activatedcollision") || n.Contains("bosstrigger") || n.Contains("counter_relay")
			|| n.Contains("set_playermode") || n.Contains("fmv") || n.Contains("ecology")
			|| n.Contains("follow_manager") || n.Contains("constraint") || n.Contains("frontend"))
		{
			return true;
		}
		return false;
	}

	// Lower-case, family name with trailing instance numbers stripped.
	private static string NameKey(string name)
	{
		string n = name.ToLowerInvariant();
		while (n.Length > 0 && (char.IsDigit(n[^1]) || n[^1] == '_'))
		{
			n = n[..^1].TrimEnd('_');
		}
		return n;
	}

	private static string? ModelFor(string objectName)
	{
		string base0 = $"project://assets/models/objects/{objectName}/{objectName}_0.gltf";
		if (Assets.ReadText(base0) != null)
		{
			return base0;
		}
		string plain = $"project://assets/models/objects/{objectName}/{objectName}.gltf";
		return Assets.ReadText(plain) != null ? plain : null;
	}
}
