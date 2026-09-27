using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using AetherCore;

namespace AetherGame;

// Contract A (logs/prefabs/DESIGN.md §2.2): the data-only descriptor scripts every Twinsanity prefab
// instance carries on its root, and the registry the managers bind them from. Descriptors hold the disc
// instance's identity, raw data and links as inspector-editable properties; all behaviour stays in the
// managers (TwinsanityLevel, CrateFx, TwinsanityWumpa, TwinsanityActors, TwinsanityCutscenes,
// TwinsanityAudio), which bind on TwinsanityLevel's second OnUpdate, when every scene-loaded script has
// attached (scripts attach in unspecified order).

/// <summary>Crate kinds the level rules handle (TwinsanityLevel), serialized as TwCrate.Kind.</summary>
public enum CrateKind { Basic, Nitro, Tnt, ExtraLife, WoodenSpring, IronSpring, Iron, Checkpoint, AkuAku, MultiHit, Level, Surprise, Detonator, Reinforced }

/// <summary>Order-independent discovery of descriptors. Static, emptied by OnDetach (Stop destroys
/// every script instance).</summary>
public static class TwRegistry
{
	private static readonly List<TwObject> s_all = new();
	private static readonly List<TwTrigger> s_triggers = new();
	private static readonly List<TwSpawn> s_spawns = new();

	public static IReadOnlyList<TwObject> All => s_all;
	public static IReadOnlyList<TwTrigger> Triggers => s_triggers;
	public static IReadOnlyList<TwSpawn> Spawns => s_spawns;

	internal static void Register(TwObject o) => s_all.Add(o);
	internal static void Unregister(TwObject o) => s_all.Remove(o);
	internal static void Register(TwTrigger t) => s_triggers.Add(t);
	internal static void Unregister(TwTrigger t) => s_triggers.Remove(t);
	internal static void Register(TwSpawn s) => s_spawns.Add(s);
	internal static void Unregister(TwSpawn s) => s_spawns.Remove(s);

	/// <summary>The descriptor on an entity root, or null when it carries none.</summary>
	public static TwObject? Of(Entity e) => e.IsValid ? e.GetScript<TwObject>() : null;

	internal static float[] Floats(string csv)
	{
		if (csv.Length == 0)
		{
			return Array.Empty<float>();
		}
		string[] parts = csv.Split(',');
		var values = new float[parts.Length];
		for (int i = 0; i < parts.Length; i++)
		{
			values[i] = float.Parse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture);
		}
		return values;
	}

	internal static int[] Ints(string csv)
	{
		if (csv.Length == 0)
		{
			return Array.Empty<int>();
		}
		string[] parts = csv.Split(',');
		var values = new int[parts.Length];
		for (int i = 0; i < parts.Length; i++)
		{
			values[i] = int.Parse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture);
		}
		return values;
	}

	// World positions of the root's children named "<prefix> 0..n", in index order (converter-written).
	internal static Vector3[] Children(Entity root, string prefix)
	{
		var found = new SortedDictionary<int, Vector3>();
		for (int i = 0; i < root.ChildCount; i++)
		{
			Entity child = root.GetChild(i);
			string name = child.Name;
			if (name.Length > prefix.Length + 1 && name.StartsWith(prefix, StringComparison.Ordinal) && name[prefix.Length] == ' '
				&& int.TryParse(name.AsSpan(prefix.Length + 1), NumberStyles.None, CultureInfo.InvariantCulture, out int index))
			{
				found[index] = child.Position;
			}
		}
		var points = new Vector3[found.Count];
		found.Values.CopyTo(points, 0);
		return points;
	}
}

/// <summary>Base descriptor: the disc instance's identity and raw data. Data only - OnUpdate is not overridden.</summary>
public abstract class TwObject : EntityScript
{
	public string Area = "";        // chunk stem ("beach", "huba", "hubb"...): scripts.json and cutscene zones
	public int Layer = -1, Id = -1; // disc (layer, id); -1 = hand-placed (not a cutscene agent)
	public int ObjectId;            // disc object id (CrateFx state tables, scripts.json object table)
	public string ObjectName = "";  // disc instance name (InstanceName); dispatch key for BehaviourOf/PushableKind/SetupOneShot
	public string Model = "";       // project:// model path the prefab was built from (CrateFx.StatePath, ModelFor)
	public int Subtype;
	public int Flags;               // uint bit pattern (unchecked cast)
	public string Floats = "";      // invariant-culture round-trip ("R") CSV of the disc floats[]
	public string Params = "";      // CSV of the disc params[]
	public Entity Link0, Link1, Link2, Link3, Link4, Link5, Link6, Link7, Link8, Link9; // disc links[i] -> instance roots

	public override void OnAttach() => TwRegistry.Register(this);
	public override void OnDetach() => TwRegistry.Unregister(this);

	/// <summary>The disc links in index order (default Entity for an empty slot).</summary>
	public Entity[] Links() => new[] { Link0, Link1, Link2, Link3, Link4, Link5, Link6, Link7, Link8, Link9 };

	/// <summary>Typed record for the managers: root world transform, parsed arrays, world points/path.</summary>
	public TwInstance Data() => new(Self, Area, Layer, Id, ObjectId, ObjectName, Model, Self.Position, Self.EulerDegrees,
		TwRegistry.Floats(Floats), unchecked((uint)Subtype), unchecked((uint)Flags), TwRegistry.Ints(Params), Links(),
		TwRegistry.Children(Self, "Point"), TwRegistry.Children(Self, "Path"));
}

public sealed class TwCrate : TwObject { public CrateKind Kind; public bool StartOpen; } // StartOpen replaces OpenStartCheckpoint's nearest-to-spawn pick
public sealed class TwActor : TwObject { }   // creatures, gems, props, pushables: dispatch stays name-keyed
public sealed class TwSpawner : TwObject { } // creature/ecology/parrot spawner; template = Link0
public sealed class TwAgent : TwObject { }   // logic-only instances (directors, text masters, DJ, ambience, sound spots...) and scripted wumpa

/// <summary>A disc trigger volume: centre = root position, rotation = root euler, extents = root scale.</summary>
public sealed class TwTrigger : EntityScript
{
	public string Area = "";
	public int Layer, Id, Header;
	public string Args = "";        // CSV of the disc args[4]
	public Entity Target0, Target1, Target2, Target3, Target4, Target5, Target6, Target7, Target8, Target9; // disc targets[] -> instance roots

	public override void OnAttach() => TwRegistry.Register(this);
	public override void OnDetach() => TwRegistry.Unregister(this);

	/// <summary>The disc targets in index order, trailing empty slots dropped.</summary>
	public Entity[] Targets()
	{
		Entity[] all = { Target0, Target1, Target2, Target3, Target4, Target5, Target6, Target7, Target8, Target9 };
		int n = all.Length;
		while (n > 0 && !all[n - 1].IsValid)
		{
			n--;
		}
		return all[..n];
	}
}

/// <summary>Crash's spawn: position = root position, facing (CrashPlayer's camera yaw) = root yaw.
/// The prefab default Floats = the beach spawn's, so any placed spawn tunes Crash correctly.</summary>
public sealed class TwSpawn : EntityScript
{
	public bool Primary;
	public string Floats = "";

	public override void OnAttach() => TwRegistry.Register(this);
	public override void OnDetach() => TwRegistry.Unregister(this);
}

/// <summary>Typed instance record replacing the JsonElement the managers read. Position, Euler, Points and
/// Path are world space.</summary>
public readonly record struct TwInstance(Entity Root, string Area, int Layer, int Id, int ObjectId, string Name,
	string Model, Vector3 Position, Vector3 Euler, float[] Floats, uint Subtype, uint Flags, int[] Params,
	Entity[] Links, Vector3[] Points, Vector3[] Path);
