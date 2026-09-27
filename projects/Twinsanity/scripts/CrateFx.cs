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
		public float Yaw;      // placed yaw; the hop's wobble tilts around it
		public float NextHop;  // seconds until the next random hop
		public bool Hopping;
		public float Vy;       // hop vertical velocity
		public float HopT;     // seconds since hop start (drives the wobble decay)
		public float SinceLand = 10.0f; // look only: seconds since the last landing (glow decay)
		public float GlowT;    // look only: glow breath clock
		public float Glow = -1.0f; // look only: emissive last written (quantised)
	}

	private sealed class Tnt
	{
		public Entity Model;
		public int ObjectId;
		public float Remaining; // counts down from 2.2 (rig: 3/2/1 at ~0.6 s each, boom at 2.2)
	}

	private static readonly System.Random s_rng = new(0x51FF);

	private static float NextHopDelay() => 0.9f + 1.4f * (float)s_rng.NextDouble();

	// Another live crate sits right on top: stacks here are 1-1.5 m apart, and a nitro above may be
	// mid-hop (+0.49 m peak), hence the 2.2 m band. Stale roots from past sessions are dropped.
	private static bool Covered(Nitro n)
	{
		List<uint>? stale = null;
		bool covered = false;
		foreach (var kv in s_crates)
		{
			if (!kv.Value.IsValid)
			{
				(stale ??= new()).Add(kv.Key);
				continue;
			}
			if (kv.Key == n.Model.Id || s_breaking.Contains(kv.Key))
			{
				continue;
			}
			Vector3 d = kv.Value.Position - n.Home;
			if (d.Y > 0.5f && d.Y < 2.2f && MathF.Abs(d.X) < 0.6f && MathF.Abs(d.Z) < 0.6f)
			{
				covered = true;
			}
		}
		stale?.ForEach(id => s_crates.Remove(id));
		return covered;
	}

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
	private static readonly Dictionary<uint, Entity> s_crates = new(); // every spawned crate root, for stack checks

	public static void Spawned(Entity crateModel, int objectId, string modelPath)
	{
		s_paths[crateModel.Id] = modelPath;
		s_crates[crateModel.Id] = crateModel;
		if (objectId == 4)
		{
			// COM_NITRO_CRATE_DEFAULT: on a condition timer the crate ApplyVelocity-hops and
			// SetWobble-tilts, then settles (states 1 -> 2 -> back). The rig (nitro2_*, 20 fps)
			// shows hops ~0.9-2.3 s apart per crate, ~0.3 s airborne: v0 = 7 with g = 50.
			s_nitros.Add(new Nitro { Model = crateModel, Home = crateModel.Position, Yaw = crateModel.EulerDegrees.Y, NextHop = NextHopDelay() });
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
		TwinsanityAudio.CrateBounce(crateModel.Position, objectId);
	}

	/// <summary>
	/// A checkpoint crate was activated. The original swaps to OGI 245 (state k2) and plays its
	/// a005 clip once (the lid drops into the crate as the sides fold down, 1.08 s), then
	/// rests on the opened OGI 246 (k3). The level crate (268) has the same layout (OGIs
	/// 974/976/975, anim 15). Idempotent per crate.
	/// </summary>
	public static void Activated(Entity crateModel, int objectId)
	{
		if (objectId is not (266 or 268) || !s_opened.Add(crateModel.Id))
		{
			return;
		}
		ShowState(crateModel, objectId, 2);
		TwinsanityAudio.Checkpoint(crateModel.Position);
		After(crateModel, PlayOnce(crateModel, "a005"), m => ShowState(m, objectId, 3));
	}

	/// <summary>
	/// Show a checkpoint crate already opened (OGI 246, state k3) with no clip and no sound: the
	/// level-start checkpoint, which the original has open on a fresh load.
	/// </summary>
	public static void ShowOpened(Entity crateModel, int objectId)
	{
		if (objectId == 266 && s_opened.Add(crateModel.Id))
		{
			ShowState(crateModel, objectId, 3);
		}
	}

	/// <summary>DETONATOR_CRATE (802) spun: COM_DETONATOR_CRATE_SPUN plays a008 (anim 1172) before it
	/// detonates. Returns the clip's length.</summary>
	public static float DetonatorSpun(Entity crateModel) => PlayOnce(crateModel, "a008");

	/// <summary>COM_DETONATOR_CRATE_DETONATE: the plunger goes down (a009, anim 1173) and stays.</summary>
	public static void Detonated(Entity crateModel) => PlayOnce(crateModel, "a009");

	/// <summary>A crate's clips are one-shots its scripts start (the checkpoint's a005, the detonator's
	/// a008 spin and a009 press); GENERIC_CRATE_DEFAULT plays none, so it rests in the bind pose. A
	/// loaded skinned model loops its first clip, which spun the detonator's plunger at rest. The
	/// detonator's a008 and a009 both start at that bind pose (every joint at identity), so clip 0 held
	/// at its start is the rest pose.</summary>
	public static void HoldRest(Entity crateModel)
	{
		if (Animation.ClipCount(crateModel) <= 0)
		{
			return;
		}
		SetLooping(crateModel, false);
		Animation.SetClip(crateModel, 0);
		Animation.SetTime(crateModel, 0.0f);
		Animation.SetPlaybackSpeed(crateModel, 0.0f);
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
		TwinsanityAudio.CrateBreak(crateModel.Position, objectId);
		// COM_*_CRATE_BREAK's DoParticle(0): the star-sparkle burst plays alongside the plank rig.
		CrateBreakSparkle(crateModel.Position + new Vector3(0.0f, 0.75f, 0.0f));
		bool explosive = objectId is 4 or 5;
		if (!explosive)
		{
			CrateDust(crateModel.Position + new Vector3(0.0f, 0.4f, 0.0f));
		}
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
	// Rates: the disc emits GenRate particles per frame for Emitter_OverTime frames at 60 fps
	// (capped at MaxParticleCount). Sizes are the disc's size-gradient raws * 1e-4 (the game's own cut-radius
	// formula adds MaxSize * 1e-4 as a world-space radius, which pins that scale).
	// Velocities, spawn boxes and gravity are the disc's per-second values straight across.
	private const string kParticleTex = "project://assets/particles/particle_page_{0}.png";

	/// <summary>A disc ParticleData def. <paramref name="atlas"/> (look upgrade, logs/look/FxLook/README.md) swaps the
	/// disc page sprite for a smooth fx_atlas cell of the same role when the atlas is installed; everything that
	/// sets the timing (count, rate, life, motion, keys) stays the disc's. <paramref name="lit"/> shades it as sun-lit smoke.</summary>
	private static Entity SpawnEmitter(string name, Vector3 center, string page, Vector4 uv, int count, float rate, float emitDuration,
		float life, Vector3 velocity, Vector3 velJitter, Vector3 spawnJitter, float gravityY,
		Vector4[] colorKeys, float[] alphaKeys, float[] sizeKeys, float[] rotKeys,
		int shape = 0, float radialSpeed = 0.0f, bool additive = true, int unkByte7 = 1, Vector4? atlas = null, bool lit = false, float atlasAlpha = 1.0f, Vector4[]? atlasColor = null, float atlasSize = 1.0f)
	{
		bool modern = atlas.HasValue && AtlasReady;
		Entity e = NewEmitter(name, center, modern ? kFxAtlas : string.Format(kParticleTex, page), modern ? atlas!.Value : DiscUv(uv),
			count, rate, emitDuration, life, life, velocity, velJitter, spawnJitter, gravityY, shape, radialSpeed, additive, lit && modern, 0.0f);
		if (!e.IsValid)
		{
			return e;
		}
		Particles.SetKeys(e, ParticleKeyChannel.Color, modern && atlasColor != null ? atlasColor : colorKeys);
		// The atlas cells are denser and fill more of their quad than the disc's soft page sprites; atlasAlpha and
		// atlasSize bring a swapped layer back to the disc's weight and footprint (and cut its overdraw).
		float[] alpha = (float[])alphaKeys.Clone();
		for (int i = 1; modern && i < alpha.Length; i += 2)
		{
			alpha[i] *= atlasAlpha;
		}
		SetScalarKeys(e, ParticleKeyChannel.Alpha, alpha);
		// The disc's UnkByte7 = 1 defs (EXPLODE_*_1A/1B, CRASH_DROP2, crash_LAND1) draw at twice the
		// edge of the UnkByte7 = 3 ones (EXPLODE_*_1C, CRATE_BREAK). Rig in crate widths: the nitro
		// cloud is 4.1 x 2.6 at +0.55 s and the 1C flash about 5 wide; at one scale for both, either
		// the cloud came out half as wide or the flash twice as wide (fx_nitro.png).
		SetScalarKeys(e, ParticleKeyChannel.Size, sizeKeys, (unkByte7 == 1 ? 2.0f : 1.0f) * (modern ? atlasSize : 1.0f));
		SetScalarKeys(e, ParticleKeyChannel.Rotation, rotKeys);
		return e;
	}

	/// <summary>One world-space Billboard3D emitter entity (auto-destroyed when its particles are gone). A rate of 0
	/// fires <paramref name="count"/> at once; otherwise <paramref name="rate"/> per second for <paramref name="emitDuration"/>.</summary>
	private static Entity NewEmitter(string name, Vector3 center, string texture, Vector4 uvRect, int count, float rate, float emitDuration,
		float lifeMin, float lifeMax, Vector3 velocity, Vector3 velJitter, Vector3 spawnJitter, float gravityY,
		int shape, float radialSpeed, bool additive, bool lit, float rotJitter)
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
		c.SetString("texture", texture);
		c.SetInt("space", 1); // Billboard3D
		c.SetInt("blend_mode", additive ? 1 : 0); // disc TextureFilter: Additive -> additive, Modulation -> alpha
		c.SetInt("emit_shape", shape);            // 0 box, 1 Radial, 2 ImprovedRadial (disc GenSort 6 / 11)
		c.SetFloat("radial_speed", radialSpeed);
		c.SetBool("display_space", true);         // blend after the tonemap, in gamma space, like the GS
		c.SetBool("lit", lit);
		c.SetFloat("rotation_jitter", rotJitter);
		bool burstOnly = rate <= 0.0f;
		c.SetBool("emit_on_start", burstOnly); // burst defs fire their count once, at spawn
		c.SetBool("auto_destroy", true);
		c.SetInt("burst_count", burstOnly ? count : 0);
		c.SetInt("max_particles", count);
		c.SetFloat("rate", rate);
		c.SetFloat("emit_duration", emitDuration);
		c.SetFloat("lifetime_min", lifeMin);
		c.SetFloat("lifetime_max", lifeMax);
		c.SetVector3("velocity", velocity);
		c.SetVector3("velocity_jitter", velJitter);
		c.SetVector3("spawn_jitter", spawnJitter);
		c.SetVector3("gravity_3d", new Vector3(0.0f, gravityY, 0.0f));
		c.SetVector4("uv_rect", uvRect);
		return e;
	}

	private static void SetScalarKeys(Entity e, ParticleKeyChannel channel, float[] pairs, float sizeScale = 1.0f)
	{
		var keys = new Vector4[pairs.Length / 2];
		// GS alpha: 0x80 = 1.0 (additive may exceed 1). Sizes arrive as raw * 1e-4; the engine
		// key is the quad edge (DiscSizeToEdge).
		float scale = channel == ParticleKeyChannel.Alpha ? 1.0f / 128.0f : channel == ParticleKeyChannel.Size ? DiscSizeToEdge * 1e4f * sizeScale : 1.0f;
		for (int i = 0; i < keys.Length; i++)
		{
			keys[i] = new Vector4(pairs[i * 2], pairs[i * 2 + 1] * scale, 0.0f, 0.0f);
		}
		Particles.SetKeys(e, channel, keys);
	}

	/// <summary>
	/// Disc size raw -> engine quad edge. The disc's own cull radius adds MaxSize * 1e-4 to the
	/// particle extents, so raw * 1e-4 is the half-extent and the edge is twice that. Defs with
	/// UnkByte7 = 1 draw twice that again (see SpawnEmitter).
	/// </summary>
	public const float DiscSizeToEdge = 2.0e-4f;

	/// <summary>
	/// A disc ParticleData texture rect, already divided by the 128-texel page, as an engine
	/// uv_rect. The disc's V runs UP the page while particle_page_*.png is stored top-down, so
	/// v0..v1 maps to rows 1 - v1 .. 1 - v0 (crash_LAND1 then samples the grey cloud and
	/// FX_AKUTRAIL the four-point star, as the rig shows). Every particle FX goes through here.
	/// </summary>
	public static Vector4 DiscUv(Vector4 rect) => new(rect.X, 1.0f - rect.W, rect.Z, 1.0f - rect.Y);

	/// <summary>A disc colour-gradient key. GS vertex colour: 0x80 = 1.0 (255 is ~2x, overbright).</summary>
	public static Vector4 CK(float t, float r, float g, float b) => new(t, r / 128.0f, g / 128.0f, b / 128.0f);

	// ── Look upgrade: modern layers (logs/look/FxLook/README.md) ─────────────────────
	// Visual only: nothing here reads or changes gameplay, and every disc layer keeps its own
	// count, rate, life and motion, so each effect starts and ends when it always did. The layers
	// added on top are a flash core, a fireball, sun-lit smoke (the `lit` emitter flag), sparks
	// with gravity, a shock ring and a short point-light flash, all from fx_atlas.png - a
	// procedural 512 px sheet (tools/fx-atlas/fx_atlas.py) the shader filters smoothly. Without
	// the atlas every effect falls back to its disc look.
	private const string kFxAtlas = "project://assets/particles/fx_atlas.png";
	internal static readonly Vector4 FxSmoke = new(0.0f, 0.0f, 0.5f, 0.5f);
	internal static readonly Vector4 FxFire = new(0.5f, 0.0f, 1.0f, 0.5f);
	internal static readonly Vector4 FxFlash = new(0.0f, 0.5f, 0.5f, 1.0f);
	internal static readonly Vector4 FxSpark = new(0.5f, 0.5f, 0.75f, 0.75f);
	internal static readonly Vector4 FxRing = new(0.75f, 0.5f, 1.0f, 0.75f);
	internal static readonly Vector4 FxStar = new(0.5f, 0.75f, 0.75f, 1.0f);
	internal static readonly Vector4 FxEmber = new(0.75f, 0.75f, 1.0f, 1.0f);
	private static bool? s_atlasReady;

	/// <summary>Look-dev (FxPreview "legacy"): force every effect back to its disc look, for before/after strips.</summary>
	internal static bool ForceDisc;

	private static bool AtlasReady
	{
		get
		{
			if (ForceDisc)
			{
				return false;
			}
			if (s_atlasReady == null)
			{
				s_atlasReady = Assets.List(kFxAtlas).Length > 0;
				if (s_atlasReady == false)
				{
					Log.Warn($"[Twinsanity] {kFxAtlas} missing - run python tools/fx-atlas/fx_atlas.py; effects use their disc look.");
				}
			}
			return s_atlasReady.Value;
		}
	}

	/// <summary>A modern layer: sizes in metres (quad edge), alpha 0..1, colours 0..1 (above 1 brightens), all
	/// keyed over normalised age as (t, value) pairs. Skipped when the atlas is missing.</summary>
	private static Entity? Fx(string name, Vector3 center, Vector4 cell, int count, float rate, float emitDuration,
		float lifeMin, float lifeMax, Vector3 velocity, Vector3 velJitter, Vector3 spawnJitter, float gravityY,
		Vector4[] color, float[] alpha, float[] size, float spin = 0.0f, bool additive = true, bool lit = false,
		int shape = 0, float radialSpeed = 0.0f)
	{
		if (!AtlasReady)
		{
			return null;
		}
		Entity e = NewEmitter(name, center, kFxAtlas, cell, count, rate, emitDuration, lifeMin, lifeMax, velocity, velJitter, spawnJitter,
			gravityY, shape, radialSpeed, additive, lit, 180.0f);
		if (!e.IsValid)
		{
			return null;
		}
		Particles.SetKeys(e, ParticleKeyChannel.Color, color);
		RawKeys(e, ParticleKeyChannel.Alpha, alpha);
		RawKeys(e, ParticleKeyChannel.Size, size);
		RawKeys(e, ParticleKeyChannel.Rotation, new[] { 0.0f, 0.0f, 1.0f, spin });
		return e;
	}

	private static void RawKeys(Entity e, ParticleKeyChannel channel, float[] pairs)
	{
		var keys = new Vector4[pairs.Length / 2];
		for (int i = 0; i < keys.Length; i++)
		{
			keys[i] = new Vector4(pairs[i * 2], pairs[i * 2 + 1], 0.0f, 0.0f);
		}
		Particles.SetKeys(e, channel, keys);
	}

	private static Vector4 C(float t, float r, float g, float b) => new(t, r, g, b);

	/// <summary>A blast's colours: the flash tint, the fireball ramp, the smoke (start, end), the sparks and the light.</summary>
	private sealed record BlastLook(Vector4[] Flash, Vector4[] Fire, Vector3 SmokeA, Vector3 SmokeB, Vector4[] Sparks, Vector3 Light);

	private static readonly BlastLook Orange = new(
		new[] { C(0f, 1.6f, 1.5f, 1.2f), C(0.5f, 1.5f, 1.1f, 0.45f), C(1f, 1.2f, 0.5f, 0.15f) },
		new[] { C(0f, 1.5f, 1.35f, 0.8f), C(0.25f, 1.3f, 0.7f, 0.2f), C(0.6f, 0.9f, 0.3f, 0.1f), C(1f, 0.35f, 0.12f, 0.06f) },
		new Vector3(0.62f, 0.56f, 0.5f), new Vector3(1.0f, 0.97f, 0.93f),
		new[] { C(0f, 1.6f, 1.45f, 0.9f), C(0.4f, 1.4f, 0.75f, 0.2f), C(1f, 0.9f, 0.2f, 0.08f) },
		new Vector3(1.0f, 0.55f, 0.22f));

	// The nitro keeps its green: flash, fire, smoke and sparks are all nitro green.
	private static readonly BlastLook Green = new(
		new[] { C(0f, 1.3f, 1.6f, 1.25f), C(0.5f, 0.6f, 1.55f, 0.55f), C(1f, 0.2f, 1.1f, 0.25f) },
		new[] { C(0f, 1.1f, 1.6f, 0.9f), C(0.25f, 0.35f, 1.35f, 0.3f), C(0.6f, 0.12f, 0.9f, 0.18f), C(1f, 0.08f, 0.4f, 0.1f) },
		new Vector3(0.3f, 0.9f, 0.3f), new Vector3(0.62f, 1.0f, 0.55f),
		new[] { C(0f, 1.3f, 1.7f, 1.1f), C(0.4f, 0.45f, 1.5f, 0.35f), C(1f, 0.1f, 0.9f, 0.15f) },
		new Vector3(0.35f, 1.0f, 0.4f));

	/// <summary>The modern blast on top of a disc explosion (nitro, TNT, bomb; the cannon at <paramref name="scale"/> 0.45).
	/// Every layer is over by the disc cloud's own end (1.58 s) except the smoke's last thin tail (~1.9 s).</summary>
	private static void ModernBlast(Vector3 center, BlastLook look, float scale = 1.0f)
	{
		float s = scale;
		// Flash core: one white-hot bloom the first frames, gone by 0.2 s.
		Fx("BlastFlash", center, FxFlash, 1, 0.0f, 0.0f, 0.2f, 0.2f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			look.Flash, new[] { 0f, 0.85f, 0.35f, 0.7f, 1f, 0f }, new[] { 0f, 2.0f * s, 0.3f, 4.2f * s, 1f, 5.0f * s }, 25.0f);
		// Shock ring: a thin bright shell racing out over 0.3 s.
		Fx("BlastRing", center, FxRing, 1, 0.0f, 0.0f, 0.3f, 0.3f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			look.Flash, new[] { 0f, 0.8f, 0.5f, 0.45f, 1f, 0f }, new[] { 0f, 1.5f * s, 1f, 9.0f * s });
		// Fireball: turbulent billows swelling and cooling within 0.75 s.
		Fx("BlastFire", center, FxFire, 7, 0.0f, 0.0f, 0.55f, 0.75f, new Vector3(0.0f, 1.2f, 0.0f) * s, new Vector3(2.2f, 1.4f, 2.2f) * s,
			new Vector3(0.45f, 0.35f, 0.45f) * s, 1.0f, look.Fire, new[] { 0f, 0f, 0.06f, 0.95f, 0.5f, 0.7f, 1f, 0f },
			new[] { 0f, 1.4f * s, 0.3f, 3.4f * s, 1f, 4.2f * s }, 70.0f);
		// Sun-lit smoke: rises and spreads, fading in behind the fire so the blast settles into a cloud.
		Vector3 a = look.SmokeA, b = look.SmokeB;
		Fx("BlastSmoke", center, FxSmoke, (int)MathF.Max(3.0f, 6.0f * s), 0.0f, 0.0f, 1.45f, 1.9f, new Vector3(0.0f, 0.9f, 0.0f) * s,
			new Vector3(1.8f, 0.6f, 1.8f) * s, new Vector3(0.95f, 0.35f, 0.95f) * s, 0.4f,
			new[] { C(0f, a.X, a.Y, a.Z), C(0.35f, (a.X + b.X) * 0.5f, (a.Y + b.Y) * 0.5f, (a.Z + b.Z) * 0.5f), C(1f, b.X, b.Y, b.Z) },
			new[] { 0f, 0f, 0.18f, 0f, 0.38f, 1.0f, 0.7f, 0.75f, 1f, 0f }, new[] { 0f, 1.8f * s, 0.4f, 3.4f * s, 1f, 4.4f * s }, 40.0f,
			additive: false, lit: true);
		// Sparks: hot embers flung out and falling.
		Fx("BlastSparks", center, FxSpark, (int)MathF.Max(6.0f, 22.0f * s), 0.0f, 0.0f, 0.45f, 0.8f, new Vector3(0.0f, 4.5f, 0.0f) * s,
			new Vector3(7.0f, 3.5f, 7.0f) * s, new Vector3(0.3f) * s, -12.0f, look.Sparks, new[] { 0f, 1.0f, 0.7f, 0.9f, 1f, 0f },
			new[] { 0f, 0.45f * s, 1f, 0.12f * s });
		LightFlash(center + new Vector3(0.0f, 0.6f, 0.0f), look.Light, 30.0f * s, 9.0f * MathF.Sqrt(s), 0.45f);
	}

	// A short point-light flash (tiled, no shadow): intensity falls off quadratically over its life.
	private sealed class Flash
	{
		public Entity Light;
		public float Peak, Life, Age;
	}

	private static readonly List<Flash> s_flashes = new();

	private static void LightFlash(Vector3 at, Vector3 color, float intensity, float radius, float life)
	{
		Entity e = World.Create();
		e.Name = "FxLightFlash";
		e.MarkTransient();
		e.AddTransform();
		e.Position = at;
		ComponentAccess c = e.Component("Point Light");
		if (!c.Add())
		{
			e.Destroy();
			return;
		}
		c.SetVector3("color", color);
		c.SetFloat("intensity", intensity);
		c.SetFloat("radius", radius);
		c.SetBool("shadow", false);
		s_flashes.Add(new Flash { Light = e, Peak = intensity, Life = life });
	}

	private static void UpdateFlashes(float dt)
	{
		for (int i = s_flashes.Count - 1; i >= 0; i--)
		{
			Flash f = s_flashes[i];
			f.Age += dt;
			if (!f.Light.IsValid || f.Age >= f.Life)
			{
				if (f.Light.IsValid)
				{
					f.Light.Destroy();
				}
				s_flashes.RemoveAt(i);
				continue;
			}
			float k = 1.0f - f.Age / f.Life;
			f.Light.Component("Point Light").SetFloat("intensity", f.Peak * k * k);
		}
	}

	// CRATE_BREAK (bank index 0): the burst of radial star sparkles every broken crate throws.
	private static void CrateBreakSparkle(Vector3 center)
	{
		SpawnEmitter("CrateBreakFx", center, "2", new Vector4(65.9f, 2.3f, 127.8f, 62.3f) / 128.0f,
			10, 60.0f, 0.1667f, 0.2f,
			new Vector3(0.0f, 1.6f, 0.0f), new Vector3(0.25f, 0.0f, 0.125f), new Vector3(0.45f), 0.0f,
			new[] { CK(0f, 254.1f, 255f, 0f), CK(0.0745f, 237.6f, 199.7f, 50.7f), CK(0.5185f, 171.7f, 110.2f, 51.7f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 0f, 0f, 255f, 1f, 0f },
			new[] { 0f, 300.745f * 1e-4f, 0.24329f, 8878.46f * 1e-4f, 1f, 0f },
			new[] { 0f, 0f, 1f, 18f / 65536f * 360f },
			unkByte7: 3, atlas: FxStar);
		// Look: a pinch of warm splinter sparks with the stars, over within the sparkle's own 0.37 s.
		Fx("CrateBreakSparks", center, FxSpark, 8, 0.0f, 0.0f, 0.25f, 0.37f, new Vector3(0.0f, 2.2f, 0.0f), new Vector3(2.6f, 1.4f, 2.6f),
			new Vector3(0.35f), -9.0f, new[] { C(0f, 1.5f, 1.35f, 0.7f), C(1f, 1.2f, 0.55f, 0.15f) }, new[] { 0f, 1.0f, 1f, 0f },
			new[] { 0f, 0.28f, 1f, 0.08f });
	}

	/// <summary>Look only (logs/look/CrateLook/README.md): a wood-dust puff under the plank burst of a
	/// broken wooden crate. Not a disc def - the original throws only the sparkle and the fragment rig.
	/// Soft modulated sand-brown clouds (disc page 0, the landing-dust cloud), 12 of them, thrown out
	/// low and slow, swelling from 0.5 to 1.5 u over 0.9 s and settling.</summary>
	private static void CrateDust(Vector3 center)
	{
		SpawnEmitter("CrateDustFx", center, "0", new Vector4(0.0f, 63.8f, 64.2f, 128.0f) / 128.0f,
			12, 0.0f, 1.0f / 60.0f, 0.9f,
			new Vector3(0.0f, 0.7f, 0.0f), new Vector3(1.6f, 0.5f, 1.6f), new Vector3(0.4f, 0.3f, 0.4f), -0.6f,
			new[] { CK(0f, 150f, 124f, 92f), CK(1f, 120f, 104f, 84f) },
			new[] { 0f, 0f, 0.12f, 70f, 0.55f, 40f, 1f, 0f },
			new[] { 0f, 0.25f, 0.4f, 0.6f, 1f, 0.75f },
			new[] { 0f, 0f, 1f, 50f },
			additive: false, unkByte7: 3, atlas: FxSmoke, lit: true); // FxLook: smooth sun-lit atlas puff
	}

	// Look only (logs/look/CrateLook/README.md): the nitro's emissive (tw-extract's _ce mask) breathes
	// slowly and flares with the idle hop - up at take-off, held in the air, fading once it lands.
	// Knobs: NitroGlowBase (matches CrateLook.NitroGlowBase in tw-extract), NitroGlowBreath, NitroGlowHop.
	private const float NitroGlowBase = 0.12f, NitroGlowBreath = 0.08f, NitroGlowHop = 0.75f, NitroGlowFade = 0.3f;

	private static void NitroGlow(Nitro n, float dt)
	{
		n.GlowT += dt;
		if (!n.Hopping)
		{
			n.SinceLand += dt;
		}
		float hop = n.Hopping ? MathF.Min(n.HopT / 0.06f, 1.0f) : MathF.Exp(-n.SinceLand / NitroGlowFade);
		float breath = 0.5f + 0.5f * MathF.Sin(2.0f * MathF.PI * 0.7f * n.GlowT + n.Home.X);
		float k = MathF.Round((NitroGlowBase + NitroGlowBreath * breath + NitroGlowHop * hop) * 32.0f) / 32.0f; // quantised: fewer material rewrites
		if (k == n.Glow)
		{
			return;
		}
		n.Glow = k;
		for (int i = 0; i < n.Model.ChildCount; i++)
		{
			Entity child = n.Model.GetChild(i);
			if (child.Name.EndsWith(" mesh", StringComparison.Ordinal))
			{
				child.Material.SetEmissive(new Vector3(k, k, k));
			}
		}
	}

	/// <summary>gen_IMPACT1 (bank index 127): the white star flash a hit throws, e.g. COM_GENERIC_CREATURE_DAMAGED_SPIN's
	/// DoParticle(0x7E81007F) when Coco spins the tutorial skunk (rig: logs/tutorial st1 ~10.45 s, st3 ~7.2 s).
	/// One particle, 0.25 s, alpha ramping to 250 in 0.034 then out by 0.85, shrinking from 15436 raw. The disc's
	/// rate 60 for 1/60 s is one particle; it is spawned as a burst because rate x dt can land a hair under 1 and
	/// emit nothing.</summary>
	public static void ImpactFlash(Vector3 center)
	{
		SpawnEmitter("ImpactFlash", center, "2", new Vector4(65.3f, 0.2f, 127.6f, 62.0f) / 128.0f,
			1, 0.0f, 1.0f / 60.0f, 0.2532225f,
			Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { CK(0f, 255f, 255f, 255f), CK(0.538215339f, 255f, 255f, 255f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 22.148859f, 0.03378904f, 250.444229f, 0.8494779f, 0f },
			new[] { 0f, 15436.44f * 1e-4f, 1f, 0f },
			new[] { 0f, 0f, 1f, 9f / 65536f * 360f },
			shape: 2, atlas: FxFlash);
		// Look: a thin shock ring with the star, same 0.25 s.
		Fx("ImpactRing", center, FxRing, 1, 0.0f, 0.0f, 0.22f, 0.22f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.4f, 1.4f, 1.4f), C(1f, 1.0f, 0.9f, 0.6f) }, new[] { 0f, 0.8f, 1f, 0f }, new[] { 0f, 0.4f, 1f, 2.4f });
	}

	/// <summary>COM_GEM_PICKUP's two DoParticle calls (logs/gemreward/particle_GEM_PICKUP_1A/1B.json): GEM_PICKUP_1A
	/// (bank 173), one white star flash shrinking over 0.28 s, and GEM_PICKUP_1B (bank 188), 30 sparks thrown out
	/// over 3 frames that fade within 0.82 s. Rig: logs/gemreward/rig_red10_sheet.png, 0.08-0.35 s after the pickup.
	/// 1B's quads are tall (53407 raw high, 5822 wide); the engine quad is square, so it takes the width.</summary>
	public static void GemPickup(Vector3 center)
	{
		SpawnEmitter("GemFlash", center, "2", new Vector4(4.1f, 35.4f, 29.4f, 61.4f) / 128.0f,
			1, 0.0f, 1.0f / 60.0f, 0.2777417f,
			Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { CK(0f, 3.77f, 4.33f, 24.09f), CK(0.2819488f, 84.63f, 88.40f, 89.77f), CK(0.8487934f, 211.7f, 211.7f, 211.7f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 0f, 0.7811512f, 255f, 1f, 255f },
			new[] { 0f, 23565.32f * 1e-4f, 1f, 2002.51f * 1e-4f },
			new[] { 0f, 41062f / 65536f * 360f, 1f, 459f / 65536f * 360f }, atlas: FxFlash, atlasAlpha: 0.75f);
		SpawnEmitter("GemSparks", center, "1", new Vector4(33.3f, 0.0f, 64.5f, 31.6f) / 128.0f,
			30, 600.0f, 3.0f / 60.0f, 0.8232403f,
			Vector3.Zero, new Vector3(3.689851f, 3.654536f, 3.691157f), new Vector3(0.1717121f, 0.187581f, 0.1676432f), 0.0f,
			new[] { CK(0f, 64f, 64f, 64f), CK(1f, 64f, 64f, 64f) },
			new[] { 0f, 0f, 0.1763019f, 246.3686f, 0.4526041f, 130.6698f, 1f, 0f },
			new[] { 0f, 5822.218f * 1e-4f, 0.1238282f, 3427.047f * 1e-4f, 1f, 0f },
			new[] { 0f, 0f, 1f, 0f }, atlas: FxSpark);
		// Look: six four-point twinkles lingering among the sparks (inside their 0.82 s), and a quick ring.
		Fx("GemTwinkles", center, FxStar, 6, 0.0f, 0.0f, 0.45f, 0.7f, new Vector3(0.0f, 0.6f, 0.0f), new Vector3(1.2f, 0.9f, 1.2f), new Vector3(0.35f),
			0.0f, new[] { C(0f, 1.6f, 1.6f, 1.6f), C(1f, 1.1f, 1.1f, 1.3f) }, new[] { 0f, 0f, 0.15f, 1.0f, 1f, 0f },
			new[] { 0f, 0.1f, 0.3f, 0.7f, 1f, 0.0f }, 90.0f);
		Fx("GemRing", center, FxRing, 1, 0.0f, 0.0f, 0.28f, 0.28f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.5f, 1.5f, 1.6f), C(1f, 1.0f, 1.0f, 1.2f) }, new[] { 0f, 0.7f, 1f, 0f }, new[] { 0f, 0.5f, 1f, 2.6f });
	}

	/// <summary>The wumpa pickup's sparkle (a look upgrade, not a disc def): one warm star flash
	/// and 16 gold-to-red sparks thrown out once, in the gem pickup's two-part style so the
	/// collectibles read as one family. Knobs: count 16, spark speed ±3.4 u/s (+1.5 up), life 0.55 s,
	/// flash edge peaking at ~1.6 u; see logs/look/WumpaLook/README.md.</summary>
	public static void WumpaPickup(Vector3 center)
	{
		SpawnEmitter("WumpaFlash", center, "2", new Vector4(65.3f, 0.2f, 127.6f, 62.0f) / 128.0f,
			1, 0.0f, 1.0f / 60.0f, 0.24f,
			Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { CK(0f, 255f, 240f, 170f), CK(0.5f, 255f, 170f, 60f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 220f, 0.3f, 200f, 1f, 0f },
			new[] { 0f, 4000f * 1e-4f, 0.25f, 8000f * 1e-4f, 1f, 2000f * 1e-4f },
			new[] { 0f, 0f, 1f, 40f },
			unkByte7: 3, atlas: FxFlash);
		SpawnEmitter("WumpaSparks", center, "1", new Vector4(33.3f, 0.0f, 64.5f, 31.6f) / 128.0f,
			16, 0.0f, 1.0f / 60.0f, 0.55f,
			new Vector3(0.0f, 1.5f, 0.0f), new Vector3(3.4f, 2.4f, 3.4f), new Vector3(0.2f), -5.0f,
			new[] { CK(0f, 255f, 235f, 140f), CK(0.4f, 255f, 150f, 40f), CK(1f, 200f, 40f, 20f) },
			new[] { 0f, 240f, 0.5f, 180f, 1f, 0f },
			new[] { 0f, 2200f * 1e-4f, 1f, 0f },
			new[] { 0f, 0f, 1f, 180f },
			unkByte7: 3, atlas: FxSpark);
		// Look: a small warm ring with the flash, same 0.24 s.
		Fx("WumpaRing", center, FxRing, 1, 0.0f, 0.0f, 0.22f, 0.22f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.6f, 1.4f, 0.8f), C(1f, 1.4f, 0.7f, 0.25f) }, new[] { 0f, 0.6f, 1f, 0f }, new[] { 0f, 0.4f, 1f, 1.8f });
	}

	/// <summary>A dead creature's pop (logs/gameplay/pop_sheet.md, defs in pop_particles.json).
	/// COM_GENERIC_CREATURE_DEAD_BODYPOP fires ENEMY_POP1 (210, sparks) and ENEMY_POP2 (211, grey
	/// puff); COM_GLOBAL_CHICKEN_POP fires SEAGULLPOP (254, feathers) and ENEMY_POP2. As for the crate
	/// explosions, the DoParticle direction words carry no velocity, so the disc Velocity/Gravity
	/// are kept as comments and only the Random_Emit / Random_Start jitter moves the particles.</summary>
	public static void CreaturePop(Vector3 center, bool feathers)
	{
		if (feathers)
		{
			SpawnEmitter("SeagullPop", center, "0", new Vector4(62.1f, 61.9f, 34.8f, 33.6f) / 128.0f,
				30, 120.0f, 15.0f / 60.0f, 0.4910268f,
				Vector3.Zero, new Vector3(5.0f), new Vector3(0.5318164f, 0.5249013f, 0.5403631f), 0.0f,
				new[] { CK(0f, 255f, 255f, 255f), CK(1f, 255f, 255f, 255f) },
				new[] { 0f, 118.193f, 1f, 59.431f },
				new[] { 0f, 2544.265f * 1e-4f, 0.511f, 2544.265f * 1e-4f, 1f, 0f },
				new[] { 0f, -293.4408f, 0.049f, 412.3929f, 1f, -293.4408f },
				additive: false);
		}
		else
		{
			SpawnEmitter("EnemyPop1", center, "1", new Vector4(33.3f, 0.0f, 64.5f, 31.6f) / 128.0f,
				20, 600.0f, 2.0f / 60.0f, 0.4687497f,
				Vector3.Zero /* Velocity 7.710999 */, new Vector3(6.420215f, 6.864955f, 6.216114f), new Vector3(0.1717121f, 0.187581f, 0.1676432f), 0.0f /* Gravity -20.77084 */,
				new[] { CK(0f, 64f, 64f, 64f), CK(0.9538402f, 64f, 64f, 64f), CK(1f, 0f, 0f, 0f) },
				new[] { 0f, 246.369f, 0.453f, 130.67f, 1f, 0f },
				new[] { 0f, 6599.096f * 1e-4f, 1f, 0f },
				new[] { 0f, 0f, 1f, 0f }, atlas: FxSpark);
		}
		SpawnEmitter("EnemyPop2", center, "0", new Vector4(1.9f, 65.3f, 64.1f, 128.0f) / 128.0f,
			18, 540.0f, 2.0f / 60.0f, 0.6184861f,
			Vector3.Zero, new Vector3(0.5481768f, 0.5494788f, 0.4915363f), new Vector3(0.7690419f, 0.7539868f, 0.7714835f), 0.0f /* Gravity 2.981393 */,
			new[] { CK(0f, 112.74633f, 112.74633f, 112.74633f), CK(0.9538402f, 64f, 64f, 64f), CK(1f, 0f, 0f, 0f) },
			new[] { 0f, 149.71f, 1f, 0f },
			new[] { 0f, 1651.204f * 1e-4f, 0.058f, 12464.893f * 1e-4f, 1f, 4646.123f * 1e-4f },
			new[] { 0f, 0f, 1f, 27165f / 65536f * 360f }, additive: !AtlasReady, atlas: FxSmoke, lit: true, atlasAlpha: 0.7f,
			atlasColor: new[] { C(0f, 1.05f, 1.05f, 1.05f), C(1f, 0.9f, 0.9f, 0.92f) }); // a white cartoon puff
		// Look: the pop's flash core and ring (the grey puff above is now sun-lit smoke), over in 0.2 s.
		Fx("PopFlash", center, FxFlash, 1, 0.0f, 0.0f, 0.16f, 0.16f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.6f, 1.6f, 1.5f), C(1f, 1.4f, 1.1f, 0.6f) }, new[] { 0f, 0.9f, 1f, 0f }, new[] { 0f, 1.0f, 0.4f, 2.0f, 1f, 2.2f }, 30.0f);
		Fx("PopRing", center, FxRing, 1, 0.0f, 0.0f, 0.22f, 0.22f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.5f, 1.5f, 1.5f), C(1f, 1.1f, 1.0f, 0.8f) }, new[] { 0f, 0.7f, 1f, 0f }, new[] { 0f, 0.5f, 1f, 2.8f });
	}

	// DoParticle(type, 0x3FFFFFC0, 0, 0, 0, 0, 0, 0, 0) - every crate explosion passes a zero
	// direction, and the disc's Velocity and Gravity act along it, so only the Random_Emit /
	// Random_Start jitter moves these particles. The rig agrees (rig_nboomr_*): the nitro cloud
	// holds in place as a big low blob for ~1.3 s and the sparks float outward instead of
	// falling (fx_nitro.png). The disc Velocity/Gravity values are kept here as comments.
	public static void Exploded(Vector3 position, int objectId)
	{
		Vector3 center = position + new Vector3(0.0f, 0.5f, 0.0f);
		TwinsanityAudio.Explosion(center);
		ExplosionFx(center, objectId == 4);
	}

	/// <summary>Look-dev (FxPreview): an explosion's visuals only, as <see cref="Exploded"/> places them.</summary>
	public static void PreviewExplosion(Vector3 position, int objectId) => ExplosionFx(position + new Vector3(0.0f, 0.5f, 0.0f), objectId == 4);

	/// <summary>Look-dev (FxPreview): a crate break's visuals only, as <see cref="Broken"/> places them.</summary>
	public static void PreviewBreak(Vector3 position)
	{
		CrateBreakSparkle(position + new Vector3(0.0f, 0.75f, 0.0f));
		CrateDust(position + new Vector3(0.0f, 0.4f, 0.0f));
	}

	/// <summary>Look-dev (FxPreview): only the modern layers of a blast.</summary>
	public static void PreviewModernBlast(Vector3 position, bool nitro) => ModernBlast(position + new Vector3(0.0f, 0.5f, 0.0f), nitro ? Green : Orange);

	/// <summary>Look-dev (FxPreview): the bomb's visuals only.</summary>
	public static void PreviewBomb(Vector3 center) => BombFx(center);

	/// <summary>Look-dev (FxPreview): the landing visuals only, as <see cref="OnCrashLanded"/> places them.</summary>
	public static void PreviewLanding(Vector3 at, bool slam)
	{
		if (slam)
		{
			CrashDrop2(at);
		}
		else
		{
			CrashLand1(at);
		}
	}

	private static void ExplosionFx(Vector3 center, bool nitro)
	{
		// 1A: the slow smoke/puff column. 1B: the fast spark/fire jet. 1C: the big flash.
		if (nitro)
		{
			SpawnEmitter("NitroExplosionA", center, "0", new Vector4(0.0f, 64.2f, 64.1f, 128.0f) / 128.0f,
				7, 60.0f, 7.0f / 60.0f, 1.582221f,
				Vector3.Zero /* Velocity 4.703004 */, new Vector3(0.4112141f, 0.0f, 0.4501139f), new Vector3(0.8801264f, 0.999f, 0.8288043f), 0.0f /* Gravity -1.353525 */,
				new[] { CK(0f, 107.821f, 234.798f, 149.144f), CK(0.241f, 0f, 247.249f, 34.105f), CK(0.623f, 0f, 174.067f, 28.473f), CK(1f, 130.208f, 113.441f, 109.646f) },
				new[] { 0f, 0f, 0.066f, 198.124f, 1f, 0f },
				new[] { 0f, 46431.45f * 1e-4f, 0.062f, 16117.997f * 1e-4f, 1f, 15893.261f * 1e-4f },
				new[] { 0f, 0f, 1f, 31154f / 65536f * 360f }, atlas: FxFire, atlasAlpha: 0.3f, atlasSize: 0.72f);
			SpawnEmitter("NitroExplosionB", center, "1", new Vector4(33.6f, 1.6f, 63.4f, 31.4f) / 128.0f,
				14, 120.0f, 7.0f / 60.0f, 0.9150347f,
				Vector3.Zero /* Velocity 16.82123 */, new Vector3(1.84683f, 2.465651f, 1.815337f), new Vector3(0.410156f, 0.6070957f, 0.4170732f), 0.0f /* Gravity -17.84222 */,
				new[] { CK(0f, 79.534f, 243.109f, 0f), CK(0.766f, 73.916f, 246.346f, 0f), CK(1f, 0f, 209.46f, 14.932f) },
				new[] { 0f, 120.687f, 0.182f, 255f, 1f, 255f },
				new[] { 0f, 17435.475f * 1e-4f, 0.049f, 5648.218f * 1e-4f, 0.798f, 2316.686f * 1e-4f, 1f, 0f },
				new[] { 0f, -7470f / 65536f * 360f, 1f, 36978f / 65536f * 360f }, atlas: FxSpark);
			SpawnEmitter("NitroExplosionC", center, "2", new Vector4(64.6f, 1.7f, 128.0f, 63.9f) / 128.0f,
				2, 0.0f, 0.0f, 0.3192643f,
				Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
				new[] { CK(0f, 118.953f, 247.716f, 148.315f), CK(0.652f, 0f, 250.3f, 47.359f), CK(1f, 0f, 171.453f, 29.05f) },
				new[] { 0f, 255f, 0.28f, 255f, 1f, 24.15f },
				new[] { 0f, 43894.496f * 1e-4f, 1f, 18781.947f * 1e-4f },
				new[] { 0f, 78299f / 65536f * 360f, 1f, 78299f / 65536f * 360f }, // track ends at its first t = 1 key
				unkByte7: 3, atlas: FxFlash, atlasAlpha: 0.6f, atlasSize: 0.8f);
		}
		else
		{
			SpawnEmitter("TntExplosionA", center, "0", new Vector4(0.0f, 64.2f, 64.1f, 128.0f) / 128.0f,
				7, 60.0f, 7.0f / 60.0f, 1.582221f,
				Vector3.Zero /* Velocity 4.703004 */, new Vector3(0.4112141f, 0.0f, 0.4501139f), new Vector3(0.8801264f, 0.999f, 0.8288043f), 0.0f /* Gravity -1.353525 */,
				new[] { CK(0f, 208.401f, 168.424f, 48.024f), CK(0.241f, 156.013f, 0f, 0f), CK(0.623f, 94.255f, 54.73f, 5.981f), CK(1f, 130.208f, 113.441f, 109.646f) },
				new[] { 0f, 0f, 0.066f, 198.124f, 1f, 0f },
				new[] { 0f, 46245.46f * 1e-4f, 0.062f, 16069.652f * 1e-4f, 0.979f, 15845.901f * 1e-4f, 1f, 26634.685f * 1e-4f },
				new[] { 0f, 0f, 1f, 31154f / 65536f * 360f }, atlas: FxFire, atlasAlpha: 0.3f, atlasSize: 0.72f);
			SpawnEmitter("TntExplosionB", center, "1", new Vector4(32.5f, 0.0f, 65.8f, 33.1f) / 128.0f,
				14, 120.0f, 7.0f / 60.0f, 0.6961219f,
				Vector3.Zero /* Velocity 16.21593 */, new Vector3(3.193508f, 2.465651f, 3.05557f), new Vector3(0.410156f, 0.6070957f, 0.4170732f), 0.0f /* Gravity -20.67021 */,
				new[] { CK(0f, 221.905f, 233.397f, 101.165f), CK(0.766f, 246.346f, 0f, 0f), CK(1f, 246.346f, 0f, 0f) },
				new[] { 0f, 120.687f, 0.182f, 255f, 1f, 255f },
				new[] { 0f, 17435.475f * 1e-4f, 0.049f, 5648.218f * 1e-4f, 0.798f, 2316.686f * 1e-4f, 1f, 0f },
				new[] { 0f, -7470f / 65536f * 360f, 1f, 36978f / 65536f * 360f }, atlas: FxSpark);
			SpawnEmitter("TntExplosionC", center, "2", new Vector4(64.6f, 1.7f, 128.0f, 63.9f) / 128.0f,
				2, 0.0f, 0.0f, 0.3192643f,
				Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
				new[] { CK(0f, 255f, 249.895f, 0f), CK(0.652f, 255f, 0f, 0f), CK(1f, 116.232f, 89.643f, 0f) },
				new[] { 0f, 255f, 0.28f, 255f, 1f, 24.15f },
				new[] { 0f, 43894.496f * 1e-4f, 1f, 18781.947f * 1e-4f },
				new[] { 0f, 78299f / 65536f * 360f, 1f, 78299f / 65536f * 360f }, // track ends at its first t = 1 key
				unkByte7: 3, atlas: FxFlash, atlasAlpha: 0.6f, atlasSize: 0.8f);
		}
		ModernBlast(center, nitro ? Green : Orange);
	}

	/// <summary>The cannon's shot (COM_RIGID_CANNON_ACTIVATED): DoParticle EXPLODE_1A, 1C and 1D
	/// (bank 13, 15, 16) at the muzzle - the rig's white-yellow flash and sparks
	/// (logs/cannon/rigfire_sheet.png, 0.1 s after the landing on the button).</summary>
	public static void MuzzleFlash(Vector3 center)
	{
		Explode1A(center);
		Explode1C(center);
		Explode1D(center);
		ModernBlast(center, Orange, 0.45f);
	}

	/// <summary>act_GLOBAL_BOMB going off (COM_GLOBAL_BOMB_DAMAGED): DoParticle EXPLODE_1A-1D
	/// (bank 13-16) and its Sounds[3] = 63 (bit-identical to the crates' 22). Defs from
	/// logs/aku/all_particles.json, mapped as the EXPLODE_TNT_* ones above (logs/cannon/emit_cs.py).</summary>
	public static void BombExploded(Vector3 center)
	{
		TwinsanityAudio.BombExplosion(center);
		BombFx(center);
	}

	private static void BombFx(Vector3 center)
	{
		Explode1A(center);
		SpawnEmitter("EXPLODE_1B", center, "1", new Vector4(1.9f, 34.3f, 30.6f, 62.4f) / 128.0f,
			21, 180f, 7.0f / 60.0f, 0.598306f,
			Vector3.Zero /* Velocity 19.73946 */, new Vector3(2.00618f, 2.81786f, 2.07087f), new Vector3(0.140788f, 1.56087f, 0.134684f), 0.0f /* Gravity -30.02218 */,
			new[] { CK(0f, 102.691f, 78.1687f, 45.5888f), CK(1f, 91.1085f, 96.2787f, 70.5784f) },
			new[] { 0f, 120.687f, 0.752277f, 120.687f, 1f, 0f },
			new[] { 0f, 5673.81f * 1e-4f, 1f, 5673.81f * 1e-4f },
			new[] { 0f, -7470f / 65536f * 360f, 1f, 36978f / 65536f * 360f }, atlas: FxSpark);
		Explode1C(center);
		Explode1D(center);
		ModernBlast(center, Orange);
	}

	private static void Explode1A(Vector3 center) =>
		SpawnEmitter("EXPLODE_1A", center, "0", new Vector4(1.8f, 1.6f, 30.4f, 30.3f) / 128.0f,
			21, 180f, 7.0f / 60.0f, 0.829455f,
			Vector3.Zero /* Velocity 20.41963 */, new Vector3(1.00561f, 4.94437f, 1.01042f), new Vector3(0.404053f, 1.45467f, 0.570475f), 0.0f /* Gravity -22.48482 */,
			new[] { CK(0f, 208.401f, 168.424f, 48.024f), CK(0.241145f, 156.013f, 0f, 0f), CK(0.622721f, 94.2554f, 54.7301f, 5.98089f), CK(1f, 130.208f, 113.441f, 109.646f) },
			new[] { 0f, 0f, 0.0661778f, 198.124f, 0.210261f, 53.0439f, 0.326627f, 42.8852f, 1f, 0f },
			new[] { 0f, 47386.5f * 1e-4f, 0.123176f, 9687.66f * 1e-4f, 1f, 7123.2f * 1e-4f },
			new[] { 0f, 0f, 1f, 23445f / 65536f * 360f }, atlas: FxFlash, atlasAlpha: 0.45f, atlasSize: 0.8f);

	private static void Explode1C(Vector3 center) =>
		SpawnEmitter("EXPLODE_1C", center, "0", new Vector4(9.4f, 74.2f, 50.3f, 120.4f) / 128.0f,
			16, 240f, 4.0f / 60.0f, 1.04305f,
			Vector3.Zero /* Velocity 9.340652 */, new Vector3(0.909934f, 4.73281f, 0.882648f), new Vector3(0f, 3.00072f, 0f), 0.0f /* Gravity -6.271156 */,
			new[] { CK(0f, 154.581f, 131.617f, 111.769f), CK(1f, 154.581f, 131.617f, 111.769f) },
			new[] { 0f, 0f, 0.260937f, 32.0575f, 1f, 0f },
			new[] { 0f, 7905.12f * 1e-4f, 0.87285f, 22005.6f * 1e-4f, 1f, 7905.12f * 1e-4f },
			new[] { 0f, 0f, 1f, 24157f / 65536f * 360f },
			unkByte7: 3, atlas: FxFire, atlasAlpha: 0.5f);

	private static void Explode1D(Vector3 center) =>
		SpawnEmitter("EXPLODE_1D", center, "2", new Vector4(66.8f, 66.4f, 126f, 126.1f) / 128.0f,
			2, 0.0f, 0.0f, 0.101817f,
			Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { CK(0f, 255f, 249.895f, 0f), CK(0.245475f, 255f, 0f, 0f), CK(1f, 116.232f, 89.6426f, 0f) },
			new[] { 0f, 255f, 1f, 24.1496f },
			new[] { 0f, 33711.6f * 1e-4f, 1f, 50000f * 1e-4f },
			new[] { 0f, 18f / 65536f * 360f, 1f, 18f / 65536f * 360f },
			unkByte7: 3, atlas: FxFlash, atlasAlpha: 0.6f, atlasSize: 0.8f);

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
			ShapeRadial, 5.471634f, atlas: FxSpark);
		// Look: a ring of sun-lit sand dust thrown out flat with the spark ring, and a quick flash core.
		Fx("SlamDust", at + new Vector3(0.0f, 0.15f, 0.0f), FxSmoke, 16, 0.0f, 0.0f, 0.55f, 0.8f, Vector3.Zero,
			new Vector3(0.1f, 180.0f, 4.0f), new Vector3(0.6f, 180.0f, -84.0f), 0.3f,
			new[] { C(0f, 1.0f, 0.9f, 0.7f), C(1f, 0.92f, 0.84f, 0.7f) }, new[] { 0f, 0f, 0.08f, 0.85f, 0.5f, 0.55f, 1f, 0f },
			new[] { 0f, 0.5f, 0.35f, 1.3f, 1f, 1.7f }, 60.0f, additive: false, lit: true, shape: ShapeRadial, radialSpeed: 3.2f);
		Fx("SlamFlash", at + new Vector3(0.0f, 0.3f, 0.0f), FxFlash, 1, 0.0f, 0.0f, 0.14f, 0.14f, Vector3.Zero, Vector3.Zero, Vector3.Zero, 0.0f,
			new[] { C(0f, 1.6f, 1.2f, 1.6f), C(1f, 1.5f, 1.3f, 0.6f) }, new[] { 0f, 0.8f, 1f, 0f }, new[] { 0f, 1.2f, 1f, 2.6f }, 20.0f);
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
			ShapeImprovedRadial, 0.9808426f, additive: false, atlas: FxSmoke, lit: true, atlasAlpha: 1.2f,
			atlasColor: new[] { C(0f, 1.0f, 0.9f, 0.72f), C(1f, 0.86f, 0.79f, 0.66f) }); // sand, not the disc's grey
	}

	// ── Spin swirl (look only) ───────────────────────────────────────────────────
	// The spin model's own streak ring stays the swirl; around it, warm motes flung off the ring
	// and (on the ground) a skirt of sun-lit sand dust, emitted for exactly the spin's length and
	// carried along with Crash.
	private sealed class Swirl
	{
		public CrashPlayer? Crash;
		public Entity Emitter;
		public float Lift;
	}

	private static readonly List<Swirl> s_swirls = new();

	public static void SpinSwirl(CrashPlayer crash, float length, bool grounded)
	{
		SpinSwirlAt(crash.Self.Position, length, grounded, crash);
	}

	/// <summary>Look-dev (FxPreview): the swirl standing at <paramref name="at"/>.</summary>
	public static void SpinSwirlAt(Vector3 at, float length, bool grounded, CrashPlayer? crash = null)
	{
		const float Lift = 0.75f;
		if (Fx("SpinMotes", at + new Vector3(0.0f, Lift, 0.0f), FxSpark, 24, 60.0f, length, 0.16f, 0.24f, Vector3.Zero,
			new Vector3(0.1f, 180.0f, 25.0f), new Vector3(0.85f, 180.0f, -90.0f), 0.0f,
			new[] { C(0f, 1.6f, 1.4f, 0.9f), C(1f, 1.5f, 0.6f, 0.15f) }, new[] { 0f, 0.9f, 1f, 0f }, new[] { 0f, 0.3f, 1f, 0.05f },
			shape: ShapeRadial, radialSpeed: 3.0f) is Entity motes)
		{
			s_swirls.Add(new Swirl { Crash = crash, Emitter = motes, Lift = Lift });
		}
		if (grounded && Fx("SpinDust", at + new Vector3(0.0f, 0.1f, 0.0f), FxEmber, 14, 35.0f, length, 0.4f, 0.55f, new Vector3(0.0f, 0.6f, 0.0f),
			new Vector3(0.1f, 180.0f, 6.0f), new Vector3(0.45f, 180.0f, -84.0f), 0.0f,
			new[] { C(0f, 1.0f, 0.9f, 0.7f), C(1f, 0.92f, 0.84f, 0.7f) }, new[] { 0f, 0f, 0.1f, 0.7f, 1f, 0f }, new[] { 0f, 0.35f, 1f, 0.9f },
			90.0f, additive: false, lit: true, shape: ShapeRadial, radialSpeed: 2.4f) is Entity dust)
		{
			s_swirls.Add(new Swirl { Crash = crash, Emitter = dust, Lift = 0.1f });
		}
	}

	private static void UpdateSwirls()
	{
		for (int i = s_swirls.Count - 1; i >= 0; i--)
		{
			Swirl s = s_swirls[i];
			if (!s.Emitter.IsValid)
			{
				s_swirls.RemoveAt(i);
				continue;
			}
			if (s.Crash != null)
			{
				s.Emitter.Position = s.Crash.Self.Position + new Vector3(0.0f, s.Lift, 0.0f);
			}
		}
	}

	/// <summary>Swap a crate's visible model to OGI state slot <paramref name="k"/>.</summary>
	private static void ShowState(Entity crate, int objectId, int k)
	{
		if (StatePath(crate, objectId, k) is string path)
		{
			LoadState(crate, path);
		}
	}

	/// <summary>The model file of a crate's OGI state slot <paramref name="k"/>, or null when it has none.</summary>
	public static string? StatePath(Entity crate, int objectId, int k)
	{
		// The crate's own model ("<Name>_0.gltf"); per-instance models (act_TNTCRATE3...) differ
		// from the object table's.
		string? model = s_paths.GetValueOrDefault(crate.Id) ?? CrateFragments.ObjectModel(objectId);
		int under = model?.LastIndexOf('_') ?? -1;
		if (k < 0 || under < 0 || !model!.EndsWith(".gltf", StringComparison.OrdinalIgnoreCase))
		{
			return null;
		}
		return model[..(under + 1)] + k + ".gltf";
	}

	internal static void LoadState(Entity crate, string path)
	{
		// Entity.LoadModel only DETACHES the previous model's meshes, leaving them standing in
		// the world as orphans: every swap left the old state behind (the "crate inside an
		// opened crate" duplicate). Destroy them first - but not the crate's collider, which
		// is a child of the same root ("<Kind> Crate Body"): the TNT countdown swap took it and
		// Crash dropped through the lit TNT to the ground under it (logs/hubrun/tnt_wedge.txt).
		for (int i = crate.ChildCount - 1; i >= 0; i--)
		{
			Entity child = crate.GetChild(i);
			if (!child.Name.EndsWith(" Crate Body", StringComparison.Ordinal))
			{
				child.Destroy();
			}
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
		FxPreview.Poll();
		UpdateFlashes(dt);
		UpdateSwirls();
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
			NitroGlow(n, dt);
			if (!n.Hopping)
			{
				n.NextHop -= dt;
				// A nitro with a crate resting on it stays put (original: stacked nitros never hop).
				if (n.NextHop <= 0.0f && Covered(n))
				{
					n.NextHop = NextHopDelay();
				}
				else if (n.NextHop <= 0.0f)
				{
					n.Hopping = true;
					n.Vy = 7.0f;
					n.HopT = 0.0f;
					TwinsanityAudio.NitroHop(n.Model.Position);
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
				n.Model.EulerDegrees = new Vector3(0.0f, n.Yaw, 0.0f);
				n.Hopping = false;
				n.SinceLand = 0.0f;
				n.NextHop = NextHopDelay();
				continue;
			}
			n.Model.Position = pos;
			float decay = MathF.Max(0.0f, 1.0f - n.HopT / 0.35f);
			float tilt = 0.15f * MathF.Sin(25.13f * n.HopT) * decay;
			n.Model.EulerDegrees = new Vector3(tilt, n.Yaw, -tilt);
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
				if (nowState < 6)
				{
					TwinsanityAudio.TntTick(t.Model.Position);
				}
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
