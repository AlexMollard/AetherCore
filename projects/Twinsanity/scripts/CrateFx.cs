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
/// directly (no clip exists); the explosion look is still an approximation - the disc's
/// explosion particle bank sits in CRASH.BD's global data and is not extracted yet.
/// ponytail: hand-tuned explosion look + guessed fuse-state timing, upgrade path =
/// extracted particle definitions and script-derived state durations.
/// </summary>
public static class CrateFx
{
	private sealed class Nitro
	{
		public Entity Model;
		public float Phase;
	}

	private sealed class Tnt
	{
		public Entity Model;
		public int ObjectId;
		public float Remaining; // counts down from 3; model states swap each second
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

	public static void Spawned(Entity crateModel, int objectId, string modelPath)
	{
		s_paths[crateModel.Id] = modelPath;
		// Nitro: the original plays a single short clip on its one joint (anim slot 4);
		// with no extractable clip the shiver stands in for it.
		if (objectId == 4)
		{
			s_nitros.Add(new Nitro { Model = crateModel, Phase = 0.0f });
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
			// Jumping on TNT arms the 3 s fuse; the crate's OGI states count it down.
			s_tnts.Add(new Tnt { Model = crateModel, ObjectId = objectId, Remaining = 3.0f });
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

	public static void Exploded(Vector3 position, int objectId)
	{
		bool nitro = objectId == 4;
		Vector3 center = position + new Vector3(0.0f, 0.5f, 0.0f);
		Entity e = World.Create();
		e.Name = nitro ? "NitroExplosion" : "TntExplosion";
		e.MarkTransient();
		e.AddTransform();
		e.Position = center;
		var c = e.Component("Particle Emitter");
		if (c.Add())
		{
			// The original's explosion is a fast radial burst of camera-facing puffs:
			// TNT orange fading to dark smoke, nitro bright green. Emitted in the
			// camera-facing vertical plane, which is how the game's billboards read
			// from its mostly axis-aligned chase camera.
			c.SetFloat("rate", 0.0f);
			c.SetInt("burst_count", nitro ? 40 : 32);
			c.SetBool("emit_on_start", true);
			c.SetBool("auto_destroy", true);
			c.SetFloat("lifetime_min", 0.45f);
			c.SetFloat("lifetime_max", 0.8f);
			c.SetFloat("speed_min", nitro ? 4.5f : 3.5f);
			c.SetFloat("speed_max", nitro ? 7.0f : 5.5f);
			c.SetFloat("direction_deg", 90.0f);
			c.SetFloat("spread_deg", 90.0f);
			c.SetVector2("gravity", new Vector2(0.0f, nitro ? -1.5f : -4.0f));
			c.SetFloat("start_size", nitro ? 0.9f : 0.8f);
			c.SetFloat("end_size", 1.6f);
			Vector4 hot = nitro ? new Vector4(0.35f, 1.0f, 0.25f, 0.95f) : new Vector4(1.0f, 0.55f, 0.15f, 0.95f);
			Vector4 cool = nitro ? new Vector4(0.15f, 0.5f, 0.1f, 0.0f) : new Vector4(0.35f, 0.3f, 0.28f, 0.0f);
			c.SetVector4("start_color", hot);
			c.SetVector4("end_color", cool);
			c.SetInt("blend_mode", 1); // additive
		}
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
	}

	public static void Update(float dt)
	{
		// Nitro idle shiver (stand-in for its unextractable single-joint clip).
		for (int i = s_nitros.Count - 1; i >= 0; i--)
		{
			Nitro n = s_nitros[i];
			if (!n.Model.IsValid || s_breaking.Contains(n.Model.Id))
			{
				s_nitros.RemoveAt(i);
				continue;
			}
			n.Phase += dt * 22.0f;
			float w = 1.0f + 0.06f * MathF.Sin(n.Phase);
			n.Model.Scale = new Vector3(w, 2.0f - w, w);
		}

		// TNT fuse: OGI states k2 (3), k3 (2), k4 (1), then k5 blinking for the last
		// quarter second, matching the original's 12/11/10/9 countdown graphics.
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
			int prevState = prev > 2.0f ? 2 : prev > 1.0f ? 3 : prev > 0.25f ? 4 : 5;
			int nowState = t.Remaining > 2.0f ? 2 : t.Remaining > 1.0f ? 3 : t.Remaining > 0.25f ? 4 : (int)(t.Remaining * 20.0f) % 2 == 0 ? 5 : 2;
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
