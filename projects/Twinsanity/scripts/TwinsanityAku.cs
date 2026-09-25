using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Aku Aku: the mask that floats beside Crash, driven by TwinsanityLevel's mask count. Everything
/// here is presentation plus the invincibility timer; TwinsanityLevel owns the count and the hits.
///
/// Rig evidence (logs/aku/, PCSX2 on the PAL ISO, mask object read from EE memory at 0xd2c930):
/// - Crash's health word (helper+20 bits 6-13) is 1 + masks: a fresh beach reads 2 (one mask, the
///   mask already beside him), and a crab took 3 -> 2 -> 1 -> dead (track_hitcrab.csv). Respawn
///   reads 2 again. With no mask the object is parked ~10 u above Crash, out of view.
/// - Gaining the first mask (track_pop.csv): it drops from ~22 u above Crash onto his shoulder.
///   Following is a plain chase: speed ~3.2-3.5 x distance (running at 9 u/s it trails 2.6 u),
///   capped ~55 u/s. Rest offset: 0.7 to Crash's right, 2.0 above his feet; it turns with him.
/// - The bob is the model's own clip a000 (joint0 Y, 0.254 u over 3.92 s, same period on the rig).
/// - Two masks: FX_AKUTRAIL sparkles (Default.rm2 ParticleData 143, logs/aku/particles.json).
/// - A third crate at two masks: the mask moves onto his face (0.33 in front, 1.7 up, no bob) for
///   ~8.1 s wrapped in the INVULNERABLE1-3 aura (ParticleData 154/153/152), then it flies back to
///   his side and he still has two masks (track_l3.csv).
///   Nothing hurts him meanwhile, and crabs he walks through do NOT die (_invwalk.png).
/// - Losing a mask from two: the mask jolts up ~0.9 and back ~1 and settles (0.8 s). Losing the
///   last one: it dips to his feet, then shoots up (~15 u/s) behind him and is gone.
/// Sounds: AKUMASK Sounds[] = [59, 200, 59, 200, 59, -, 200] with no DoSound in its scripts, so the
/// engine picks them; 59 on a gain and 200 on a loss is read from that slot pattern [not rig-confirmed].
/// </summary>
public static class TwinsanityAku
{
	private const string ModelPath = "project://assets/models/objects/AKUMASK/AKUMASK.gltf";

	private const float SideOffset = 0.7f;
	private const float UpOffset = 2.0f;
	private const float FaceForward = 0.33f;
	private const float FaceUp = 1.7f;
	private const float ChaseRate = 3.5f;   // 1/s
	private const float MaxSpeed = 55.0f;
	private const float FaceRate = 10.0f;
	private const float DropHeight = 22.0f;
	private const float InvincibleSeconds = 8.1f;
	private const float KickSeconds = 0.8f;
	private const float FlyAwaySeconds = 1.0f;
	private const float YawOffset = 180.0f; // the mask model faces +Z like Crash's

	private static Entity s_mask;
	private static Entity s_trail;
	private static Entity[] s_invulnerable = Array.Empty<Entity>();
	private static int s_shown;          // mask count the visuals are showing
	private static float s_invincible;   // seconds of invincibility left
	private static float s_kick = -1.0f; // time since a two->one loss
	private static float s_fly = -1.0f;  // time since the last mask was lost
	private static Vector3 s_flyFrom;
	private static float s_yaw;

	public static bool Invincible => s_invincible > 0.0f;

	/// <summary>Level build: forget the last session's state (statics outlive Play).</summary>
	public static void Start()
	{
		s_shown = 0;
		s_invincible = 0.0f;
		s_kick = -1.0f;
		s_fly = -1.0f;
	}

	/// <summary>An Aku crate broke: returns the new mask count (a third mask is 8 s of
	/// invincibility on top of two).</summary>
	public static int Collect(int masks)
	{
		TwinsanityAudio.AkuGained();
		if (masks >= 2)
		{
			s_invincible = InvincibleSeconds;
			return 2;
		}
		return masks + 1;
	}

	/// <summary>Per frame, after the rules ran: follow Crash and show <paramref name="masks"/>.</summary>
	public static void Update(float dt, CrashPlayer crash, int masks)
	{
		Entity body = crash.Self;
		float r = crash.Facing * (MathF.PI / 180.0f);
		Vector3 forward = new(-MathF.Sin(r), 0.0f, -MathF.Cos(r));
		Vector3 right = new(-forward.Z, 0.0f, forward.X);
		Vector3 feet = body.Position;
		if (!s_mask.IsValid)
		{
			// Level start: the mask Crash starts with is already at his side (rig beach_orig).
			Create(feet + right * SideOffset + new Vector3(0.0f, UpOffset, 0.0f));
			s_mask.SetActive(masks > 0);
			s_shown = masks;
			s_yaw = crash.Facing;
		}
		if (masks == 0)
		{
			s_invincible = 0.0f;
		}
		else if (s_invincible > 0.0f)
		{
			s_invincible -= dt;
		}

		if (masks != s_shown)
		{
			if (masks > s_shown && s_shown == 0)
			{
				// The first mask drops in from high above him.
				s_fly = -1.0f;
				s_mask.SetActive(true);
				s_mask.Position = feet + right * SideOffset + new Vector3(0.0f, DropHeight, 0.0f);
				s_yaw = crash.Facing;
			}
			else if (masks < s_shown && masks > 0)
			{
				s_kick = 0.0f;
			}
			else if (masks == 0)
			{
				s_fly = 0.0f;
				s_flyFrom = s_mask.Position;
			}
			s_shown = masks;
		}

		bool face = Invincible;
		SetBob(!face);
		SetEmitting(s_trail, masks >= 2 && !face);
		foreach (Entity e in s_invulnerable)
		{
			SetEmitting(e, face);
			e.Position = feet + new Vector3(0.0f, 0.9f, 0.0f); // body centre [the disc's anchor joint is not known]
		}

		if (s_fly >= 0.0f)
		{
			// Losing the last mask: it dips to his feet, then accelerates up and away behind him.
			s_fly += dt;
			float t = s_fly;
			float up = t < 0.1f ? 0.3f : 0.3f + 3.5f * (t - 0.1f) + 6.0f * (t - 0.1f) * (t - 0.1f);
			s_mask.Position = new Vector3(s_flyFrom.X, feet.Y + up, s_flyFrom.Z) - forward * (2.0f * MathF.Min(t, 1.0f));
			if (s_fly > FlyAwaySeconds)
			{
				s_fly = -1.0f;
				s_mask.SetActive(false);
			}
			s_trail.Position = s_mask.Position;
			return;
		}
		if (masks == 0)
		{
			return;
		}

		Vector3 target;
		float rate;
		if (face)
		{
			target = feet + forward * FaceForward + new Vector3(0.0f, FaceUp, 0.0f);
			rate = FaceRate;
		}
		else
		{
			target = feet + right * SideOffset + new Vector3(0.0f, UpOffset, 0.0f);
			rate = ChaseRate;
			if (s_kick >= 0.0f)
			{
				// Two -> one: the mask jolts up and back, then settles (rig: +0.9 up at 0.4 s).
				s_kick += dt;
				float k = MathF.Sin(MathF.PI * MathF.Min(s_kick / KickSeconds, 1.0f));
				target += (new Vector3(0.0f, 0.95f, 0.0f) - forward * 1.0f + right * 0.7f) * k;
				if (s_kick >= KickSeconds)
				{
					s_kick = -1.0f;
				}
			}
		}
		Vector3 pos = s_mask.Position;
		Vector3 step = (target - pos) * (1.0f - MathF.Exp(-rate * dt));
		float max = MaxSpeed * dt;
		if (step.Length() > max)
		{
			step = Vector3.Normalize(step) * max;
		}
		s_mask.Position = pos + step;
		float dy = ((crash.Facing - s_yaw + 540.0f) % 360.0f) - 180.0f;
		s_yaw += dy * (1.0f - MathF.Exp(-10.0f * dt));
		s_mask.EulerDegrees = new Vector3(0.0f, s_yaw + YawOffset, 0.0f);
		s_trail.Position = s_mask.Position;
	}

	private static void Create(Vector3 at)
	{
		s_mask = World.Create();
		s_mask.Name = "AkuAku";
		s_mask.MarkTransient();
		s_mask.AddTransform();
		s_mask.Position = at;
		s_mask.LoadModel(ModelPath);
		s_mask.SetActive(false);
		s_bobOn = null;
		s_trail = AkuTrail();
		s_invulnerable = Invulnerable();
	}

	private static bool? s_bobOn;

	private static void SetBob(bool on)
	{
		if (s_bobOn == on)
		{
			return;
		}
		s_bobOn = on;
		int clip = Animation.Find(s_mask, "a000");
		if (clip < 0)
		{
			return;
		}
		Animation.SetClip(s_mask, clip);
		Animation.SetTime(s_mask, 0.0f);
		Animation.SetPlaybackSpeed(s_mask, on ? 1.0f : 0.0f); // on his face the mask holds still
	}

	private static void SetEmitting(Entity e, bool on)
	{
		ComponentAccess c = e.Component("Particle Emitter");
		if (c.Exists && c.GetBool("emitting") != on)
		{
			c.SetBool("emitting", on);
		}
	}

	private const float kRaw2Deg = 360.0f / 65536.0f;

	// One disc ParticleData def as a looping world-space Billboard3D emitter (switched with
	// "emitting"). Conventions as in CrateFx: sizes are raws * 1e-4, GS alpha and colour 0x80 = 1.0
	// (CrateFx.CK), disc texture rects through CrateFx.DiscUv (the disc's V runs up the page).
	// Rates: a GenRate of n > 0 emits n per 60 Hz frame, n < 0 one every -n frames; with the
	// def's life that reproduces its MaxParticleCount (143: 12/s * 0.739 s = 9).
	private static Entity Emitter(string name, int page, Vector4 discUv, int max, int genRate, float life,
		Vector3 velJitter, Vector3 spawnJitter, float gravity, Vector4[] color, float[] alpha, float[] size,
		float rotJitter = 0.0f, float[]? rotation = null)
	{
		Entity e = World.Create();
		e.Name = name;
		e.MarkTransient();
		e.AddTransform();
		ComponentAccess c = e.Component("Particle Emitter");
		if (!c.Add())
		{
			return e;
		}
		c.SetString("texture", $"project://assets/particles/particle_page_{page}.png");
		c.SetInt("space", 1);      // Billboard3D, simulated in world space: the sparkles stay behind
		c.SetInt("blend_mode", 1); // TextureFilter Additive (all four defs)
		c.SetInt("emit_shape", 0);
		c.SetBool("display_space", true); // blend after the tonemap, in gamma space, like the GS
		c.SetBool("emit_on_start", false);
		c.SetBool("auto_destroy", false);
		c.SetBool("emitting", false);
		c.SetInt("burst_count", 0);
		c.SetInt("max_particles", max);
		c.SetFloat("rate", genRate > 0 ? genRate * 60.0f : 60.0f / -genRate);
		c.SetFloat("emit_duration", 0.0f);
		c.SetFloat("lifetime_min", life);
		c.SetFloat("lifetime_max", life);
		c.SetVector3("velocity", Vector3.Zero);
		c.SetVector3("velocity_jitter", velJitter);
		c.SetVector3("spawn_jitter", spawnJitter);
		c.SetVector3("gravity_3d", new Vector3(0.0f, gravity, 0.0f));
		c.SetVector4("uv_rect", CrateFx.DiscUv(discUv / 128.0f));
		c.SetFloat("rotation_jitter", rotJitter);
		Particles.SetKeys(e, ParticleKeyChannel.Color, color);
		Keys(e, ParticleKeyChannel.Alpha, alpha, 1.0f / 128.0f);
		Keys(e, ParticleKeyChannel.Size, size, CrateFx.DiscSizeToEdge);
		if (rotation != null)
		{
			Keys(e, ParticleKeyChannel.Rotation, rotation, kRaw2Deg);
		}
		return e;
	}

	private static void Keys(Entity e, ParticleKeyChannel channel, float[] pairs, float scale)
	{
		var keys = new Vector4[pairs.Length / 2];
		for (int i = 0; i < keys.Length; i++)
		{
			keys[i] = new Vector4(pairs[i * 2], pairs[i * 2 + 1] * scale, 0.0f, 0.0f);
		}
		Particles.SetKeys(e, channel, keys);
	}

	// FX_AKUTRAIL (143): the two-mask sparkle trail. No launch speed, +-(0.44, 0.36, 0.43)
	// jitter, drifting UP at 1.09 u/s^2, random start angle (MinRotation -360 .. 360).
	private static Entity AkuTrail() => Emitter("AkuTrail", 0, new Vector4(31.5f, 0.0f, 63.5f, 32.5f), 9, -5, 0.7390902f,
		new Vector3(0.4399409f, 0.3626297f, 0.4266761f), new Vector3(0.3045322f, 0.4556634f, 0.314876f), 1.08949f,
		new[] { CrateFx.CK(0f, 230.408936f, 230.408936f, 230.408936f), CrateFx.CK(0.256964952f, 219.3407f, 225.054932f, 0f),
			CrateFx.CK(0.373372138f, 238.128662f, 55.28818f, 0f), CrateFx.CK(0.688475966f, 0f, 205.53772f, 143.063492f), CrateFx.CK(1f, 32.2201424f, 139.359741f, 0f) },
		new[] { 0f, 0f, 0.050846383f, 255f, 1f, 255f },
		new[] { 0f, 0f, 0.08671884f, 3572.638f, 0.2673171f, 1859.25146f, 1f, 0f },
		360.0f);

	// INVULNERABLE1-3 (154, 153, 152): the invincibility aura. The three share a spawn box of
	// +-(0.24, 1.281, 0.241) around his body; 1 and 2 also jitter +-(1.07, 0.40, 1.01) u/s.
	private static Entity[] Invulnerable()
	{
		Vector3 box = new(0.24f, 1.281f, 0.241f);
		Vector3 jitter = new(1.074632f, 0.3974609f, 1.012984f);
		return new[]
		{
			Emitter("AkuInvulnerable1", 0, new Vector4(0.0f, 32.1f, 32.8f, 63.9f), 56, 2, 0.471124f, jitter, box, 0.0f,
				new[] { CrateFx.CK(0f, 13.0426025f, 0f, 0f), CrateFx.CK(0.5f, 95.4071045f, 73.58175f, 0f), CrateFx.CK(1f, 113.749741f, 121.554565f, 0f) },
				new[] { 0f, 108.3992f, 1f, 0f },
				new[] { 0.020703122f, 0f, 0.0479817577f, 15943.15f, 1f, 0f },
				87.72614f, new[] { 0f, 15969f, 1f, 967f }),
			Emitter("AkuInvulnerable2", 1, new Vector4(33.3f, 0.0f, 64.5f, 31.4f), 15, -2, 0.4930964f, jitter, box, 0.0f,
				new[] { CrateFx.CK(0f, 13.0426025f, 0f, 0f), CrateFx.CK(0.0472657122f, 237.724f, 158.269714f, 0f), CrateFx.CK(1f, 113.749741f, 121.554565f, 0f) },
				new[] { 0f, 0f, 0.04439961f, 190.094238f, 1f, 0f },
				new[] { 0.020703122f, 0f, 0.06651899f, 6348.422f, 0.143814787f, 2470.00586f, 1f, 0f }),
			Emitter("AkuInvulnerable3", 0, new Vector4(65.1f, 0.0f, 127.9f, 63.7f), 17, -4, 1.112561f, Vector3.Zero, box, 0.0f,
				new[] { CrateFx.CK(0f, 13.0426025f, 0f, 0f), CrateFx.CK(0.6694638f, 237.724f, 183.3422f, 0f), CrateFx.CK(1f, 24.304533f, 153.865356f, 0f) },
				new[] { 0f, 0f, 0.0238249786f, 88.99453f, 1f, 0f },
				new[] { 0.020703122f, 0f, 0.0552559979f, 1957.38867f, 0.113932118f, 761.5689f, 1f, 1957.38867f }),
		};
	}
}
