using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Crate presentation: OGI model-state animation, bounce squash, break debris and
/// TNT / nitro explosions. LevelGameplay owns the rules and calls these hooks; nothing
/// here decides gameplay.
///
/// Crate models are single-joint, so the disc holds no skeletal clips for them: the
/// original animates crates by swapping OGI model states. Each extracted
/// models/objects/&lt;Name&gt;_&lt;k&gt;.gltf is one OGI state in first-distinct order; the
/// sequences below were dumped from the RM2 object tables (logs/crate-objs.txt), and are
/// identical across every level that defines the kind:
///   BASICCRATE (3)      OGIs [4,5,-,28,-,-,-,5]      -&gt; k0 whole, k1 broken-open (28 = shared puff)
///   NITROCRATE (4)      OGIs [6,-,-,28,7,-,-,-,1127] -&gt; k0 idle, k2 triggered
///   TNTCRATE (5)        OGIs [8,-,-,28,12,-,-,12,11,10,9,1126] -> k2..k5 fuse countdown 3-2-1
///   CHECKPOINT (266)    OGIs [29,-,-,28,-,245,-,-,246] -> k2/k3 activated top
///   EXTRALIFE (12)      OGIs [19,5,-,28,-,-,-,5,24]  -> k0 whole, k1 broken-open
///   IRONSPRING (14)     OGIs [21,-,30,28,...]        -> k2 compressed spring
/// The bounce squash is transform scaling because the behaviour scripts drive the joint
/// directly (no clip exists). The break sparkle, TNT and nitro explosion particles, the
/// landing dust and the nitro hop are driven from the disc's own values (ParticleData in
/// Startup/Default.rm2, pages in assets/particles/, defs dumped in logs/cratebreak/particles.json).
/// </summary>
public static class CrateFx
{
	private sealed class Nitro
	{
		public Entity Model;
		public Vector3 Home;   // rest position; the hop lands back here
		public float NextHop;  // seconds until the next random hop
		public bool Hopping;
		public float Vy;       // hop vertical velocity
		public float HopT;     // seconds since hop start (drives the wobble decay)
	}

	private sealed class Tnt
	{
		public Entity Model;
		public int ObjectId;
		public float Remaining; // counts down from 2.2 (rig: 3/2/1 at ~0.6 s each, boom at 2.2)
	}

	private static readonly System.Random s_rng = new(0x51FF);

	private static float NextHopDelay() => 0.9f + 1.4f * (float)s_rng.NextDouble();

	private sealed class Squash
	{
		public Entity Model;
		public float T;
	}

	private static readonly HashSet<uint> s_breaking = new();       // crate models playing their break
	private static readonly Dictionary<string, string> s_clipStates = new(); // "<KIND>_<k>_<clip>" -> path carrying it ("" = none)
	private static readonly List<Nitro> s_nitros = new();
	private static readonly List<Tnt> s_tnts = new();
	private static readonly List<Squash> s_squashes = new();
	private sealed class Timer
	{
		public Entity Model;
		public float Left;
		public Action<Entity> Action = _ => { };
	}

	private static readonly HashSet<uint> s_opened = new();
	private static readonly List<Timer> s_timers = new();
	private static readonly Dictionary<uint, string> s_paths = new(); // crate root -> its "<Name>_0.gltf"

	public static void Spawned(Entity crateModel, int objectId, string modelPath)
	{
		s_paths[crateModel.Id] = modelPath;
		if (objectId == 4)
		{
			// COM_NITRO_CRATE_DEFAULT: on a condition timer the crate ApplyVelocity-hops and
			// SetWobble-tilts, then settles (states 1 -> 2 -> back). The rig (nitro2_*, 20 fps)
			// shows hops ~0.9-2.3 s apart per crate, ~0.3 s airborne: v0 = 7 with g = 50.
			s_nitros.Add(new Nitro { Model = crateModel, Home = crateModel.Position, NextHop = NextHopDelay() });
		}
	}

	/// <summary>A crate's model rest position changed (a stacked crate fell); nitro hops land here.</summary>
	public static void Moved(Entity crateModel, Vector3 home)
	{
		foreach (Nitro n in s_nitros)
		{
			if (n.Model.Id == crateModel.Id)
			{
				n.Home = home;
				if (!n.Hopping)
				{
					n.Model.Position = home;
				}
			}
		}
	}

	public static void Bounced(Entity crateModel, int objectId)
	{
		if (s_breaking.Contains(crateModel.Id))
		{
			return;
		}
		if (objectId == 5)
		{
			// Jumping on TNT arms the 2.2 s fuse on the same tick as the bounce; the crate's
			// OGI states count it down (3 / 2 / 1, then the lit blink). A bounce arc lands
			// back on the crate, so guard against re-arming a fuse already running.
			bool armed = false;
			foreach (Tnt t in s_tnts)
			{
				if (t.Model.Id == crateModel.Id)
				{
					armed = true;
					break;
				}
			}
			if (!armed)
			{
				s_tnts.Add(new Tnt { Model = crateModel, ObjectId = objectId, Remaining = 2.2f });
				// The countdown starts on this tick: show the first digit now, not on the
				// first state CHANGE (which would leave the crate idle through the "3").
				ShowState(crateModel, objectId, 3);
			}
		}
		s_squashes.Add(new Squash { Model = crateModel, T = 0.0f });
	}

	/// <summary>
	/// A checkpoint crate was activated. The original swaps to OGI 245 (state k2) and plays its
	/// a005 clip once (the lid drops into the crate as the sides fold down, 1.08 s), then
	/// rests on the opened OGI 246 (k3). Idempotent per crate.
	/// </summary>
	public static void Activated(Entity crateModel, int objectId)
	{
		if (objectId != 266 || !s_opened.Add(crateModel.Id))
		{
			return;
		}
		ShowState(crateModel, objectId, 2);
		After(crateModel, PlayOnce(crateModel, "a005"), m => ShowState(m, objectId, 3));
	}

	/// <summary>Play a crate state's clip once from the start; returns its length (0 = no clip).</summary>
	private static float PlayOnce(Entity crate, string clip)
	{
		int index = Animation.Find(crate, clip);
		if (index < 0)
		{
			return 0.0f;
		}
		SetLooping(crate, false);
		Animation.SetClip(crate, index);
		Animation.SetTime(crate, 0.0f);
		Animation.SetPlaybackSpeed(crate, 1.0f);
		return Animation.ClipDuration(crate);
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

	private static void After(Entity model, float seconds, Action<Entity> action)
		=> s_timers.Add(new Timer { Model = model, Left = seconds, Action = action });

	/// <summary>
	/// A crate broke: <paramref name="crateModel"/> is handed over and destroyed here once its
	/// break has played. The original never spawns debris objects - it swaps the crate to a
	/// ten-plank fragment OGI whose own clip throws the planks out and shrinks them to nothing
	/// (COM_BASIC_CRATE_BREAK et al. in Startup/Default.rm2):
	///   wooden crates  OGI 5 (k1), clip a007 = anim 9, 0.6 s radial burst
	///   nitro          OGI 7 (k2), clip a004 = anim 10
	///   TNT            OGI 12 (k2), clip a004 = anim 11
	/// </summary>
	public static void Broken(Entity crateModel, int objectId)
	{
		if (!s_breaking.Add(crateModel.Id))
		{
			return;
		}
		crateModel.Scale = Vector3.One; // drop any squash / shiver in progress
		// COM_*_CRATE_BREAK's DoParticle(0): the star-sparkle burst plays alongside the plank rig.
		CrateBreakSparkle(crateModel.Position + new Vector3(0.0f, 0.75f, 0.0f));
		bool explosive = objectId is 4 or 5;
		float length = ShowStateWithClip(crateModel, objectId, explosive ? 2 : 1, explosive ? "a004" : "a007");
		if (length <= 0.0f)
		{
			// No fragment rig for this kind (iron, level crates...): it simply vanishes.
			crateModel.Destroy();
			return;
		}
		After(crateModel, length, m => m.Destroy());
	}

	/// <summary>
	/// Swap to state <paramref name="k"/> and play <paramref name="clip"/> once; returns the clip
	/// length, 0 when no extracted copy of the state carries it. The object-table copies
	/// (BASICCRATE, NITROCRATE) were extracted from chunks that lack the break clips, while the
	/// Hub's own act_ copies of the same OGI have them, so fall back to those.
	/// </summary>
	private static float ShowStateWithClip(Entity crate, int objectId, int k, string clip)
	{
		ShowState(crate, objectId, k);
		float length = PlayOnce(crate, clip);
		if (length > 0.0f)
		{
			return length;
		}
		string? own = s_paths.GetValueOrDefault(crate.Id) ?? CrateFragments.ObjectModel(objectId);
		if (own == null)
		{
			return 0.0f;
		}
		string kind = StateKind(own);
		string key = $"{kind}_{k}_{clip}";
		if (!s_clipStates.TryGetValue(key, out string? found))
		{
			found = "";
			foreach (string listed in Assets.List($"project://assets/models/objects/act_{kind}*/act_{kind}*_{k}.gltf"))
			{
				// Assets.List answers project-relative paths; LoadModel needs the mount prefix.
				string candidate = listed.Contains("://") ? listed : "project://" + listed.TrimStart('/');
				// Read the glTF's JSON rather than loading every candidate model (seconds each).
				if (Assets.ReadText(candidate)?.Contains($"\"name\":\"{clip}\"", StringComparison.Ordinal) == true)
				{
					found = candidate;
					break;
				}
			}
			s_clipStates[key] = found;
		}
		if (found.Length == 0)
		{
			return 0.0f;
		}
		LoadState(crate, found);
		return PlayOnce(crate, clip);
	}

	// "…/act_TNTCRATE3/act_TNTCRATE3_0.gltf" -> "TNTCRATE"
	private static string StateKind(string path)
	{
		string file = path[(path.LastIndexOf('/') + 1)..];
		string stem = file[..file.LastIndexOf('_')];
		if (stem.StartsWith("act_", StringComparison.Ordinal))
		{
			stem = stem[4..];
		}
		return stem.TrimEnd('0', '1', '2', '3', '4', '5', '6', '7', '8', '9');
	}

	// ── Disc particle bank (Startup/Default.rm2 ParticleData) ─────────────────────
	// Rates: the disc emits GenRate * MaxCount particles across Emitter_OverTime frames at
	// 60 fps. Sizes are the disc's size-gradient raws * 1e-4 (the game's own cut-radius
	// formula adds MaxSize * 1e-4 as a world-space radius, which pins that scale).
	// Velocities, spawn boxes and gravity are the disc's per-second values straight across.
	private const string kParticleTex = "project://assets/particles/particle_page_{0}.png";

	private static Entity SpawnEmitter(string name, Vector3 center, string page, Vector4 uv, int count, float rate, float emitDuration,
		float life, Vector3 velocity, Vector3 velJitter, Vector3 spawnJitter, float gravityY,
		Vector4[] colorKeys, float[] alphaKeys, float[] sizeKeys, float[] rotKeys,
		int shape = 0, float radialSpeed = 0.0f, bool additive = true)
	{
		Entity e = World.Create();
		e.Name = name;
		e.MarkTransient();
		e.AddTransform();
		e.Position = center;
		var c = e.Component("Particle Emitter");
		if (!c.Add())
		{
			e.Destroy();
			return e;
		}
		c.SetString("texture", string.Format(kParticleTex, page));
		c.SetInt("space", 1); // Billboard3D
		c.SetInt("blend_mode", additive ? 1 : 0); // disc TextureFilter: Additive -> additive, Modulation -> alpha
		c.SetInt("emit_shape", shape);            // 0 box, 1 Radial, 2 ImprovedRadial (disc GenSort 6 / 11)
		c.SetFloat("radial_speed", radialSpeed);
		bool burstOnly = rate <= 0.0f;
		c.SetBool("emit_on_start", burstOnly); // burst defs fire their count once, at spawn
		c.SetBool("auto_destroy", true);
		c.SetInt("burst_count", burstOnly ? count : 0);
		c.SetInt("max_particles", count);
		c.SetFloat("rate", rate);
		c.SetFloat("emit_duration", emitDuration);
		c.SetFloat("lifetime_min", life);
		c.SetFloat("lifetime_max", life);
		c.SetVector3("velocity", velocity);
		c.SetVector3("velocity_jitter", velJitter);
		c.SetVector3("spawn_jitter", spawnJitter);
		c.SetVector3("gravity_3d", new Vector3(0.0f, gravityY, 0.0f));
		c.SetVector4("uv_rect", uv);
		Particles.SetKeys(e, ParticleKeyChannel.Color, colorKeys);
		SetScalarKeys(e, ParticleKeyChannel.Alpha, alphaKeys);
		SetScalarKeys(e, ParticleKeyChannel.Size, sizeKeys);
		SetScalarKeys(e, ParticleKeyChannel.Rotation, rotKeys);
		return e;
	}

	private static void SetScalarKeys(Entity e, ParticleKeyChannel channel, float[] pairs)
	{
		var keys = new Vector4[pairs.Length / 2];
		float scale = channel == ParticleKeyChannel.Alpha ? 1.0f / 128.0f : 1.0f; // GS alpha: 0x80 = 1.0, additive may exceed 1
		for (int i = 0; i < keys.Length; i++)
		{
			keys[i] = new Vector4(pairs[i * 2], pairs[i * 2 + 1] * scale, 0.0f, 0.0f);
		}
		Particles.SetKeys(e, channel, keys);
	}

	private static Vector4 CK(float t, float r, float g, float b) => new(t, r / 255.0f, g / 255.0f, b / 255.0f);

	// CRATE_BREAK (bank index 0): the burst of radial star sparkles every broken crate throws.
	private static void CrateBreakSparkle(Vector3 center)
	{
		SpawnEmitter("CrateBreakFx", center, "2", new Vector4(65.9f, 2.3f, 127.8f, 62.3f) / 128.0f,
			10, 60.0f, 0.1667f, 0.2f,
			new Vector3(0.0f, 1.6f, 0.0f), new Vector3(0.25f, 0.0f, 0.125f), new Vector3(0.45f), 0.0f,
			new[] { CK(0f, 254.1f, 255f, 0f), CK(0.0745f, 237.6f, 199.7f, 50.7f), CK(0.5185f, 171.7f, 110.2f, 51.7f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 0f, 0f, 255f, 1f, 0f },
			new[] { 0f, 300.745f * 1e-4f, 0.24329f, 8878.46f * 1e-4f, 1f, 0f },
			new[] { 0f, 0f, 1f, 18f / 65536f * 360f });
	}

	public static void Exploded(Vector3 position, int objectId)
	{
		Vector3 center = position + new Vector3(0.0f, 0.5f, 0.0f);
		bool nitro = objectId == 4;
		// 1A: the slow smoke/puff column. 1B: the fast spark/fire jet. 1C: the big flash.
		if (nitro)
		{
			SpawnEmitter("NitroExplosionA", center, "0", new Vector4(0.0f, 64.2f, 64.1f, 128.0f) / 128.0f,
				7, 60.0f, 7.0f / 60.0f, 1.582221f,
				new Vector3(0.0f, 4.703004f, 0.0f), new Vector3(0.4112141f, 0.0f, 0.4501139f), new Vector3(0.8801264f, 0.999f, 0.8288043f), -1.353525f,
				new[] { CK(0f, 107.821f, 234.798f, 149.144f), CK(0.241f, 0f, 247.249f, 34.105f), CK(0.623f, 0f, 174.067f, 28.473f), CK(1f, 130.208f, 113.441f, 109.646f) },
				new[] { 0f, 0f, 0.066f, 198.124f, 1f, 0f },
				new[] { 0f, 46431.45f * 1e-4f, 0.062f, 16117.997f * 1e-4f, 1f, 15893.261f * 1e-4f },
				new[] { 0f, 0f, 1f, 31154f / 65536f * 360f });
			SpawnEmitter("NitroExplosionB", center, "1", new Vector4(33.6f, 1.6f, 63.4f, 31.4f) / 128.0f,
				14, 120.0f, 7.0f / 60.0f, 0.9150347f,
				new Vector3(0.0f, 16.82123f, 0.0f), new Vector3(1.84683f, 2.465651f, 1.815337f), new Vector3(0.410156f, 0.6070957f, 0.4170732f), -17.84222f,
				new[] { CK(0f, 79.534f, 243.109f, 0f), CK(0.766f, 73.916f, 246.346f, 0f), CK(1f, 0f, 209.46f, 14.932f) },
				new[] { 0f, 120.687f, 0.182f, 255f, 1f, 255f },
				new[] { 0f, 17435.475f * 1e-4f, 0.049f, 5648.218f * 1e-4f, 0.798f, 2316.686f * 1e-4f, 1f, 0f },
				new[] { 0f, -7470f / 65536f * 360f, 1f, 36978f / 65536f * 360f });
			SpawnEmitter("NitroExplosionC", center, "2", new Vector4(64.6f, 1.7f, 128.0f, 63.9f) / 128.0f,
				2, 0.0f, 0.0f, 0.3192643f,
				Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
				new[] { CK(0f, 118.953f, 247.716f, 148.315f), CK(0.652f, 0f, 250.3f, 47.359f), CK(1f, 0f, 171.453f, 29.05f) },
				new[] { 0f, 255f, 0.28f, 255f, 1f, 24.15f },
				new[] { 0f, 43894.496f * 1e-4f, 1f, 18781.947f * 1e-4f },
				new[] { 0f, 78299f / 65536f * 360f, 1f, 78299f / 65536f * 360f }); // track ends at its first t = 1 key
		}
		else
		{
			SpawnEmitter("TntExplosionA", center, "0", new Vector4(0.0f, 64.2f, 64.1f, 128.0f) / 128.0f,
				7, 60.0f, 7.0f / 60.0f, 1.582221f,
				new Vector3(0.0f, 4.703004f, 0.0f), new Vector3(0.4112141f, 0.0f, 0.4501139f), new Vector3(0.8801264f, 0.999f, 0.8288043f), -1.353525f,
				new[] { CK(0f, 208.401f, 168.424f, 48.024f), CK(0.241f, 156.013f, 0f, 0f), CK(0.623f, 94.255f, 54.73f, 5.981f), CK(1f, 130.208f, 113.441f, 109.646f) },
				new[] { 0f, 0f, 0.066f, 198.124f, 1f, 0f },
				new[] { 0f, 46245.46f * 1e-4f, 0.062f, 16069.652f * 1e-4f, 0.979f, 15845.901f * 1e-4f, 1f, 26634.685f * 1e-4f },
				new[] { 0f, 0f, 1f, 31154f / 65536f * 360f });
			SpawnEmitter("TntExplosionB", center, "1", new Vector4(32.5f, 0.0f, 65.8f, 33.1f) / 128.0f,
				14, 120.0f, 7.0f / 60.0f, 0.6961219f,
				new Vector3(0.0f, 16.21593f, 0.0f), new Vector3(3.193508f, 2.465651f, 3.05557f), new Vector3(0.410156f, 0.6070957f, 0.4170732f), -20.67021f,
				new[] { CK(0f, 221.905f, 233.397f, 101.165f), CK(0.766f, 246.346f, 0f, 0f), CK(1f, 246.346f, 0f, 0f) },
				new[] { 0f, 120.687f, 0.182f, 255f, 1f, 255f },
				new[] { 0f, 17435.475f * 1e-4f, 0.049f, 5648.218f * 1e-4f, 0.798f, 2316.686f * 1e-4f, 1f, 0f },
				new[] { 0f, -7470f / 65536f * 360f, 1f, 36978f / 65536f * 360f });
			SpawnEmitter("TntExplosionC", center, "2", new Vector4(64.6f, 1.7f, 128.0f, 63.9f) / 128.0f,
				2, 0.0f, 0.0f, 0.3192643f,
				Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
				new[] { CK(0f, 255f, 249.895f, 0f), CK(0.652f, 255f, 0f, 0f), CK(1f, 116.232f, 89.643f, 0f) },
				new[] { 0f, 255f, 0.28f, 255f, 1f, 24.15f },
				new[] { 0f, 43894.496f * 1e-4f, 1f, 18781.947f * 1e-4f },
				new[] { 0f, 78299f / 65536f * 360f, 1f, 78299f / 65536f * 360f }); // track ends at its first t = 1 key
		}
	}

	// ── Crash landing FX ─────────────────────────────────────────────────────────
	// Both straight from the disc's ParticleData (Startup/Default.rm2, dump in
	// logs/cratefx2/particles_land.json). Body slam = CRASH_DROP2 (49): GenSort Radial, 85
	// additive sparks on page 1 born 1.1 u out on a horizontal ring (polar -84.6 deg) flying
	// outward at 5.47 u/s, huge (2.89 u) and pink at birth, gold by 0.12 of their 0.53 s life,
	// shrinking to 0.31 u - the rig's pink flash then gold ring (rig_sslam_*). High landing =
	// crash_LAND1 (129): ImprovedRadial, 30 modulated grey puffs rising at 0.78 u/s^2. An
	// ordinary jump landing throws nothing (rig_jump_sheet); the fall speed that switches the
	// dust on is [UNVERIFIED] (24 u/s ~ a 6 u drop with the game's g = 50).
	private const float LandDustMinFall = 24.0f;
	private const int ShapeRadial = 1, ShapeImprovedRadial = 2;
	private const float kRaw2Deg = 360.0f / 65536.0f;

	public static void HookCrash()
	{
		CrashPlayer.Landed -= OnCrashLanded; // idempotent across play sessions
		CrashPlayer.Landed += OnCrashLanded;
	}

	private static void OnCrashLanded(CrashPlayer crash, float impact, bool slam)
	{
		Vector3 at = crash.Self.Position;
		if (slam)
		{
			CrashDrop2(at);
		}
		else if (impact >= LandDustMinFall)
		{
			CrashLand1(at);
		}
	}

	private static void CrashDrop2(Vector3 at)
	{
		SpawnEmitter("CrashDrop2", at, "1", new Vector4(32.3f, 0.0f, 65.3f, 32.5f) / 128.0f,
			85, 17.0f * 60.0f, 5.0f / 60.0f, 0.5315906f,
			Vector3.Zero, new Vector3(0.0f, 32768f * kRaw2Deg, 0.0f), new Vector3(1.095946f, 32768f * kRaw2Deg, -15391f * kRaw2Deg), 0.0f,
			new[] { CK(0f, 194.175659f, 0f, 255f), CK(0.119661361f, 255f, 238.68895f, 0f), CK(1f, 94.72229f, 0f, 0f) },
			new[] { 0f, 32.3074951f, 0.02695064f, 176.109512f, 0.184894964f, 36.8630524f, 0.457681119f, 58.45761f, 1f, 0f },
			new[] { 0f, 28900.1016f * 1e-4f, 0.096484974f, 7762.684f * 1e-4f, 1f, 3095.451f * 1e-4f },
			new[] { 0f, 0f, 1f, 17f * kRaw2Deg },
			ShapeRadial, 5.471634f);
	}

	private static void CrashLand1(Vector3 at)
	{
		SpawnEmitter("CrashLand1", at, "0", new Vector4(0.0f, 63.8f, 64.2f, 128.0f) / 128.0f,
			30, 15.0f * 60.0f, 2.0f / 60.0f, 0.7392572f,
			Vector3.Zero, new Vector3(0.4752603f, 32768f * kRaw2Deg, 0.0f), new Vector3(0.75651f, 32768f * kRaw2Deg, 3467f * kRaw2Deg), 0.7759857f,
			new[] { CK(0f, 254.25293f, 254.25293f, 254.25293f), CK(1f, 63.28308f, 63.28308f, 63.28308f) },
			new[] { 0f, 0f, 0.115623765f, 39.66123f, 1f, 0f },
			new[] { 0f, 5537.58838f * 1e-4f, 1f, 5537.58838f * 1e-4f },
			new[] { 0f, 20245f * kRaw2Deg, 1f, 39834f * kRaw2Deg },
			ShapeImprovedRadial, 0.9808426f, additive: false);
	}

	/// <summary>Swap a crate's visible model to OGI state slot <paramref name="k"/>.</summary>
	private static void ShowState(Entity crate, int objectId, int k)
	{
		// The crate's own model ("<Name>_0.gltf"); per-instance models (act_TNTCRATE3...) differ
		// from the object table's.
		string? model = s_paths.GetValueOrDefault(crate.Id) ?? CrateFragments.ObjectModel(objectId);
		int under = model?.LastIndexOf('_') ?? -1;
		if (k < 0 || under < 0 || !model!.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase))
		{
			return;
		}
		LoadState(crate, model[..(under + 1)] + k + ".gltf");
	}

	private static void LoadState(Entity crate, string path)
	{
		// Entity.LoadModel only DETACHES the previous model's meshes, leaving them standing in
		// the world as orphans: every swap left the old state behind (the "crate inside an
		// opened crate" duplicate). Destroy them first; the crate root carries nothing else.
		for (int i = crate.ChildCount - 1; i >= 0; i--)
		{
			crate.GetChild(i).Destroy();
		}
		crate.LoadModel(path);
	}

	/// <summary>Called by TwinsanityLevel so fragment spawns reuse the object table.</summary>
	public static void RegisterObjectModels(Dictionary<int, string> models)
	{
		// Once per level build (after the crates spawned): statics outlive a Play session.
		s_opened.Clear();
		s_timers.Clear();
		s_breaking.Clear();
		foreach (var kv in models)
		{
			CrateFragments.Models[kv.Key] = kv.Value;
		}
		HookCrash();
	}

	public static void Update(float dt)
	{
		// Nitro idle: the original's random hop-and-wobble (ApplyVelocity + SetWobble on a
		// condition timer), not a clip. Wobble: the disc calls SetWobble with (freq, amp) =
		// (25.13, 0.15) rad / rad on X and Z at opposite phase, so the crate rocks side to
		// side while airborne and settles when it lands.
		for (int i = s_nitros.Count - 1; i >= 0; i--)
		{
			Nitro n = s_nitros[i];
			if (!n.Model.IsValid || s_breaking.Contains(n.Model.Id))
			{
				s_nitros.RemoveAt(i);
				continue;
			}
			if (!n.Hopping)
			{
				n.NextHop -= dt;
				if (n.NextHop <= 0.0f)
				{
					n.Hopping = true;
					n.Vy = 7.0f;
					n.HopT = 0.0f;
				}
				continue;
			}
			n.HopT += dt;
			Vector3 pos = n.Model.Position;
			pos.Y += n.Vy * dt;
			n.Vy -= 50.0f * dt;
			if (n.Vy < 0.0f && pos.Y <= n.Home.Y)
			{
				// Landed: settle and schedule the next random hop.
				n.Model.Position = n.Home;
				n.Model.EulerDegrees = Vector3.Zero;
				n.Hopping = false;
				n.NextHop = NextHopDelay();
				continue;
			}
			n.Model.Position = pos;
			float decay = MathF.Max(0.0f, 1.0f - n.HopT / 0.35f);
			float tilt = 0.15f * MathF.Sin(25.13f * n.HopT) * decay;
			n.Model.EulerDegrees = new Vector3(tilt, 0.0f, -tilt);
		}

		// TNT fuse: the rig (tnt_fuse_sheet, 44 frames from landing to the boom at ~2.2 s)
		// shows 3 / 2 / 1 for ~0.6 s each, then the lit state 1126 blinking for the last
		// stretch, then the explosion. States: k0 idle, k2 fragments, k3/k4/k5 = 3/2/1, k6 lit.
		for (int i = s_tnts.Count - 1; i >= 0; i--)
		{
			Tnt t = s_tnts[i];
			if (!t.Model.IsValid || s_breaking.Contains(t.Model.Id))
			{
				s_tnts.RemoveAt(i);
				continue;
			}
			float prev = t.Remaining;
			t.Remaining -= dt;
			int StateOf(float remaining) => remaining > 1.6f ? 3 : remaining > 1.0f ? 4 : remaining > 0.4f ? 5 : 6;
			int prevState = StateOf(prev);
			int nowState = StateOf(t.Remaining);
			if (prevState != nowState)
			{
				ShowState(t.Model, t.ObjectId, nowState);
			}
		}

		// Deferred state changes (clip ends).
		for (int i = s_timers.Count - 1; i >= 0; i--)
		{
			Timer t = s_timers[i];
			t.Left -= dt;
			if (!t.Model.IsValid || t.Left <= 0.0f)
			{
				s_timers.RemoveAt(i);
				if (t.Model.IsValid)
				{
					t.Action(t.Model);
				}
			}
		}

		// Bounce squash: down to 60% height / 120% width, spring back over ~0.25 s.
		for (int i = s_squashes.Count - 1; i >= 0; i--)
		{
			Squash s = s_squashes[i];
			s.T += dt;
			if (!s.Model.IsValid || s.T > 0.25f || s_breaking.Contains(s.Model.Id))
			{
				if (s.Model.IsValid)
				{
					s.Model.Scale = Vector3.One;
				}
				s_squashes.RemoveAt(i);
				continue;
			}
			float k = MathF.Sin(MathF.PI * Math.Min(s.T / 0.25f, 1.0f));
			s.Model.Scale = new Vector3(1.0f + 0.2f * k, 1.0f - 0.4f * k, 1.0f + 0.2f * k);
		}
	}

}

/// <summary>Object table mirror so fragment spawns reuse the level's model paths.</summary>
internal static class CrateFragments
{
	internal static readonly Dictionary<int, string> Models = new();

	internal static string? ObjectModel(int objectId) => Models.GetValueOrDefault(objectId);
}
