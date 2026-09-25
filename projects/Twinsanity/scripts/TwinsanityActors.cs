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
		Monkey,    // throws fruit from its tree at Crash
		Enemy,     // chases Crash when close; touch hurts, spin/jump/slam kills it
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
	}

	private readonly List<Actor> _actors = new();
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

		Actor a = new()
		{
			Model = e,
			Home = position,
			HomeYaw = eulerDegrees.Y,
			Angle = (objectId % 17) * 0.7f,
		};
		ReadClips(e, a);

		a.Kind = BehaviourOf(objectName);
		// One-shot props (TwinsanityProps.cs) rest until their cue; everything else loops its idle.
		if (a.Kind != Behaviour.Prop || !SetupOneShot(a, objectName, subtype))
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
			or Behaviour.Crab or Behaviour.Skunk or Behaviour.Worm or Behaviour.Monkey)
		{
			SetupCritter(a, objectName, instance, transform);
			if (a.Kind == Behaviour.Flock)
			{
				SpawnFlock(a, model);
			}
		}
		return true;
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
				a.DeathTimer -= dt;
				if (a.DeathTimer < 0.0f)
				{
					a.Alive = false;
					a.Model.Destroy();
				}
				continue;
			}
			switch (a.Kind)
			{
				case Behaviour.Flyer:
					UpdateFlyer(a, dt);
					break;
				case Behaviour.Enemy:
					UpdateEnemy(a, dt, crashPos);
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
				case Behaviour.Skunk:
					UpdateCrab(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Worm:
					UpdateWorm(a, a.Critter!, dt, crashPos);
					break;
				case Behaviour.Monkey:
					UpdateMonkey(a, a.Critter!, dt, crashPos);
					break;
			}
		}
		UpdateCritters();
		UpdateEcology(crashPos);
		_actors.AddRange(_pending);
		_pending.Clear();
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
			PlayClip(a, a.DeathClip >= 0 ? a.DeathClip : a.MoveClip);
			SetLooping(a.Model, false);
			a.DeathTimer = a.DeathClip >= 0 ? 1.0f : 0.6f;
			_host?.AddWumpa(1);
		}
		else
		{
			_host?.DamagePlayer(p, DeathKind.Generic);
		}
	}

	private void UpdatePickup(Actor a, float dt, Vector3 crashPos, ITwinsanityHost host)
	{
		a.Model.EulerDegrees = new Vector3(0.0f, a.Model.EulerDegrees.Y + 120.0f * dt, 0.0f);
		Vector3 center = crashPos + new Vector3(0.0f, 0.9f, 0.0f);
		if (Vector3.DistanceSquared(center, a.Model.Position + new Vector3(0.0f, 0.5f, 0.0f)) < PickupRadius * PickupRadius)
		{
			a.Alive = false;
			a.Model.Destroy();
			host.AddWumpa(1);
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
		if (n.StartsWith("act_global_crab"))
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
		if (n.StartsWith("act_earth_tribesman") || n.StartsWith("act_piranhaplant") || n.StartsWith("act_cortex_training_miniboss") || n.StartsWith("act_cortex_creature"))
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
