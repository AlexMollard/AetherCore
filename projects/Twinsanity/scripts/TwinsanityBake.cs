using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Editor command: builds the whole N. Sanity Beach into the live scene as ordinary, editable
/// entities - the same content TwinsanityLevel builds at play, but authored once and saved with
/// the scene as a linked prefab. Run from the editor (Twinsanity panel button or
/// aether-ctl twinsanity.bake_level); it never runs at play.
///
/// Everything lands under one root entity (kBakeRootName) and carries a "Twinsanity Marker"
/// component: `role` (a Role* constant here) says what the entity is, `identity` is the level
/// export's instance JSON rewritten into world space (position/euler/points transformed,
/// resolved model path filled in). TwinsanityLevel binds to these at play instead of reading
/// the JSON; the play path without a bake (a fresh checkout) is unchanged.
///
/// The prefab file lives in the project's gitignored assets (assets/prefabs/beach.prefab.toml);
/// the scene keeps only the instance reference plus per-entity overrides, so edits stay in git
/// and the ISO-derived geometry does not.
/// </summary>
public static class TwinsanityBake
{
	public const string BakeRootName = "Twinsanity Beach Bake";
	public const string PrefabName = "beach";

	// Twinsanity Marker `role` values (mirrored in the component's inspector tooltip and the
	// C++ bake command's report).
	public const int RoleRoot = 0;
	public const int RoleCrate = 1;
	public const int RoleWumpa = 2;
	public const int RoleActor = 3;
	public const int RoleSpawner = 4;
	public const int RoleParrotSpawner = 5;
	public const int RoleCutsceneAgent = 6;
	public const int RoleTrigger = 7;
	public const int RoleDeadlyCollision = 8;
	public const int RoleSpawn = 9;
	public const int RoleSky = 10;

	public static void Run()
	{
		Entity old = Scene.Find(BakeRootName);
		if (old.IsValid)
		{
			old.Destroy(); // a re-bake replaces the previous subtree wholesale
		}

		var cfg = new TwinsanityLevel(); // the script's property defaults are the bake's input paths
		string levelPath = cfg.LevelPath;
		string objectsPath = cfg.ObjectsPath;

		Entity root = World.Create();
		root.Name = BakeRootName;
		root.AddTransform();
		Mark(root, RoleRoot, JsonSerializer.Serialize(new Dictionary<string, string>
		{
			["level"] = levelPath,
			["objects"] = objectsPath,
		}));

		// The object model table (object id -> gltf), as TwinsanityLevel loads it.
		var objectModels = new Dictionary<int, string>();
		string? objects = Assets.ReadText(objectsPath);
		if (objects != null)
		{
			using JsonDocument table = JsonDocument.Parse(objects);
			foreach (JsonProperty row in table.RootElement.EnumerateObject())
			{
				if (row.Value.TryGetProperty("model", out JsonElement model))
				{
					objectModels[int.Parse(row.Name)] = model.GetString()!;
				}
			}
		}

		// Cutscene object ids per chunk: instances on a scripted object become cutscene-agent
		// markers even when the actor system skips them (managers, spawners, Crash duplicates).
		// Loaded lazily per chunk on first encounter (each scripts.json is a large parse).
		var cutsceneIds = new Dictionary<string, HashSet<int>>();
		var cutscenes = new TwinsanityCutscenes();
		HashSet<int> ScriptedIdsFor(string chunkPath)
		{
			if (!cutsceneIds.TryGetValue(chunkPath, out HashSet<int>? ids))
			{
				ids = cutscenes.CutsceneObjectIds(chunkPath);
				cutsceneIds[chunkPath] = ids;
			}
			return ids;
		}

		int crates = 0, wumpa = 0, actors = 0, spawners = 0, agents = 0, triggers = 0;
		string levelFolder = levelPath[..(levelPath.LastIndexOf('/') + 1)];
		var queue = new Queue<(string Path, Matrix4x4 Transform)>();
		var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		queue.Enqueue((levelPath, Matrix4x4.Identity));
		while (queue.Count > 0)
		{
			(string path, Matrix4x4 transform) = queue.Dequeue();
			if (!visited.Add(path))
			{
				continue;
			}
			string? text = Assets.ReadText(path);
			if (text == null)
			{
				Log.Warn($"[TwinsanityBake] {path} not extracted - skipped.");
				continue;
			}
			bool start = path == levelPath;
			using JsonDocument doc = JsonDocument.Parse(text);
			JsonElement rootElement = doc.RootElement;
			string name = rootElement.GetProperty("level").GetString()!;

			// Scenery chunks: models only, no bind state.
			foreach (JsonElement sceneryPath in rootElement.GetProperty("scenery").EnumerateArray())
			{
				if (Assets.List(sceneryPath.GetString()!).Length > 0)
				{
					SpawnModel(root, "Scenery", sceneryPath.GetString()!, transform.Translation, TwinsanityLevel.EulerOf(transform));
				}
			}
			if (start)
			{
				string skyPath = rootElement.GetProperty("sky").GetString()!;
				Entity sky = SpawnModel(root, "Sky", skyPath, Vector3.Zero, Vector3.Zero);
				sky.Scale = new Vector3(cfg.SkyScale);
				for (int i = 0; i < sky.ChildCount; i++)
				{
					MeshRenderer.SetCastShadows(sky.GetChild(i), false);
					// The dome is camera-centred and swallows every viewport pick otherwise.
					sky.GetChild(i).Component("Not Pickable").Add();
				}
				sky.Component("Not Pickable").Add();
				Mark(sky, RoleSky, JsonSerializer.Serialize(new Dictionary<string, string> { ["model"] = skyPath }));
			}

			// Collision pieces: every one gets its mesh body (the ground is non-deadly collision!);
			// only the deadly ones carry a marker, because that is the only play-time state. The
			// pieces are invisible gameplay hulls - Not Pickable, so viewport clicks reach the
			// visible world instead of the whole-beach hull whose AABB contains the camera.
			foreach (JsonElement piece in rootElement.GetProperty("collision").EnumerateArray())
			{
				Entity e = SpawnModel(root, "Collision", piece.GetProperty("path").GetString()!, transform.Translation, TwinsanityLevel.EulerOf(transform));
				Physics.AddMeshBody(e, piece.GetProperty("path").GetString()!);
				e.Component("Not Pickable").Add();
				for (int i = 0; i < e.ChildCount; i++)
				{
					e.GetChild(i).Component("Not Pickable").Add();
				}
				if (piece.GetProperty("deadly").GetBoolean())
				{
					Mark(e, RoleDeadlyCollision, "{}");
				}
			}

			if (start && rootElement.TryGetProperty("spawn", out JsonElement spawn))
			{
				Vector3 position = Vector3.Transform(TwinsanityLevel.Vec(spawn.GetProperty("position")), transform);
				// The same facing TwinsanityLevel derives: the instance yaw turned to the camera yaw.
				float facing = TwinsanityLevel.EulerOf(TwinsanityLevel.SysRotation(TwinsanityLevel.Vec(spawn.GetProperty("euler"))) * transform).Y + 180.0f;
				string floats = spawn.TryGetProperty("floats", out JsonElement fl) ? fl.GetRawText() : "[]";
				Entity e = World.Create();
				e.Name = "Spawn";
				e.AddTransform();
				e.SetParent(root);
				e.Position = position;
				Mark(e, RoleSpawn, "{\"facing\":" + facing.ToString(System.Globalization.CultureInfo.InvariantCulture)
					+ ",\"floats\":" + floats + "}");
			}

			foreach (JsonElement instance in rootElement.GetProperty("instances").EnumerateArray())
			{
				int objectId = instance.GetProperty("object").GetInt32();
				string objectName = instance.TryGetProperty("name", out JsonElement n) ? n.GetString()! : $"object_{objectId}";
				string? model = instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : objectModels.GetValueOrDefault(objectId);
				Vector3 position = Vector3.Transform(TwinsanityLevel.Vec(instance.GetProperty("position")), transform);
				Vector3 euler = TwinsanityLevel.EulerOf(TwinsanityLevel.SysRotation(TwinsanityLevel.Vec(instance.GetProperty("euler"))) * transform);
				string identity = IdentityJson(instance, position, euler, transform, model, path);

				// Every scripted instance is a cutscene agent whatever else it bakes to (crate,
				// wumpa, spawner): the JSON build ingests them all and directors address them by
				// (layer, id). The agent is a separate invisible point so one entity never
				// carries two identities.
				if (ScriptedIdsFor(path).Contains(objectId))
				{
					bool skipped = TwinsanityActors.Skipped(objectName);
					Entity agent = MarkerEntity(root, skipped ? objectName : objectName + " Agent", position);
					Mark(agent, RoleCutsceneAgent, identity);
					agents++;
				}

				if (objectId == 1)
				{
					if (model != null)
					{
						Entity e = SpawnModel(root, "Wumpa", model, position, new Vector3(0.0f, euler.Y, 0.0f));
						for (int i = 0; i < e.ChildCount; i++)
						{
							MeshRenderer.SetCastShadows(e.GetChild(i), false);
						}
						Mark(e, RoleWumpa, identity);
						wumpa++;
					}
					continue;
				}

				TwinsanityLevel.Kind? kind = TwinsanityLevel.KindFor(objectId, model);
				if (kind != null)
				{
					if (model == null)
					{
						continue;
					}
					// The crate model root carries the marker; its box body is a CHILD so a
					// hand-duplicate in the editor copies both, and breaking the crate (which
					// destroys the body) leaves the model for the fragment clip. The bind state
					// (kind name + disc object id) rides in the marker JSON.
					Entity crateModel = SpawnModel(root, kind.Value + " Crate", model, position, euler);
					Entity body = World.Create();
					body.Name = kind.Value + " Crate Body";
					body.AddTransform();
					body.SetParent(crateModel);
					body.Position = position + new Vector3(0.0f, 0.5f, 0.0f);
					Physics.AddBoxBody(body, new Vector3(0.5f, 0.5f, 0.5f), dynamic: false);
					Mark(crateModel, RoleCrate, IdentityJson(instance, position, euler, transform, model, null, kind.Value.ToString(), objectId));
					crates++;
					continue;
				}

				int spawnerRole = TwinsanityActors.SpawnerRoleFor(objectName);
				bool isSpawner = spawnerRole == TwinsanityActors.SpawnerParrot
					|| (spawnerRole == TwinsanityActors.SpawnerCreature
						&& instance.ValueKind == JsonValueKind.Object && instance.TryGetProperty("links", out _));
				if (isSpawner)
				{
					Entity e = MarkerEntity(root, objectName, position);
					Mark(e, spawnerRole == TwinsanityActors.SpawnerParrot ? RoleParrotSpawner : RoleSpawner, identity);
					spawners++;
					continue;
				}

				if (TwinsanityActors.Skipped(objectName))
				{
					continue;
				}

				if (model != null)
				{
					Entity e = SpawnModel(root, objectName + "#" + objectId, model, position, euler);
					if (TwinsanityActors.NameKeyOf(objectName).StartsWith("act_redwumpa"))
					{
						for (int i = 0; i < e.ChildCount; i++)
						{
							MeshRenderer.SetCastShadows(e.GetChild(i), false);
						}
					}
					Mark(e, RoleActor, identity);
					actors++;
				}
			}

			if (rootElement.TryGetProperty("triggers", out JsonElement triggersArray))
			{
				foreach (JsonElement t in triggersArray.EnumerateArray())
				{
					Vector3 center = Vector3.Transform(TwinsanityLevel.Vec(t.GetProperty("center")), transform);
					Vector3 extents = TwinsanityLevel.Vec(t.GetProperty("extents"));
					Entity e = World.Create();
					e.Name = "Trigger";
					e.AddTransform();
					e.SetParent(root);
					e.Position = center;
					e.Scale = extents;
					Mark(e, RoleTrigger, TriggerJson(t, path));
					triggers++;
				}
			}

			Log.Info($"[TwinsanityBake] chunk {name}: {crates} crates, {wumpa} wumpa, {actors} actors so far");

			if (rootElement.TryGetProperty("links", out JsonElement links))
			{
				foreach (JsonElement link in links.EnumerateArray())
				{
					string chunk = link.GetProperty("chunk").GetString()!;
					bool neighbour = link.TryGetProperty("flags", out JsonElement flags) && (flags.GetUInt32() & 0xFF) == 1;
					if (neighbour && chunk.StartsWith(levelFolder, StringComparison.OrdinalIgnoreCase))
					{
						queue.Enqueue((chunk, TwinsanityLevel.ChunkTransform(link) * transform));
					}
				}
			}
		}

		Log.Info($"[TwinsanityBake] done: {crates} crates, {wumpa} wumpa, {actors} actors, {spawners} spawners, {agents} cutscene agents, {triggers} triggers under '{BakeRootName}'");
	}

	// ---- helpers --------------------------------------------------------------------------------

	/// <summary>The instance JSON rewritten into world space: position/euler transformed, points
	/// and path keys transformed, resolved model path filled in. Cutscene markers also name their
	/// chunk (the scripts.json the play-time cutscene system must load); crate markers carry the
	/// crate kind name and disc object id for the bind.</summary>
	private static string IdentityJson(JsonElement instance, Vector3 position, Vector3 euler, Matrix4x4 transform, string? model, string? chunk,
		string? crateKind = null, int objectId = 0)
	{
		using var stream = new MemoryStream();
		using (var w = new Utf8JsonWriter(stream))
		{
			w.WriteStartObject();
			if (crateKind != null)
			{
				w.WriteString("crate", crateKind);
				w.WriteNumber("objectId", objectId);
			}
			foreach (JsonProperty p in instance.EnumerateObject())
			{
				switch (p.Name)
				{
					case "position":
						w.WritePropertyName("position");
						WriteVec(w, position);
						break;
					case "euler":
						w.WritePropertyName("euler");
						WriteVec(w, euler);
						break;
					case "points":
					case "path":
						w.WritePropertyName(p.Name);
						w.WriteStartArray();
						foreach (JsonElement pt in p.Value.EnumerateArray())
						{
							WriteVec(w, Vector3.Transform(TwinsanityLevel.Vec(pt), transform));
						}
						w.WriteEndArray();
						break;
					default:
						w.WritePropertyName(p.Name);
						p.Value.WriteTo(w);
						break;
				}
			}
			if (model != null && !instance.TryGetProperty("model", out _))
			{
				w.WriteString("model", model);
			}
			if (chunk != null)
			{
				w.WriteString("chunk", chunk);
			}
			w.WriteEndObject();
		}
		return System.Text.Encoding.UTF8.GetString(stream.ToArray());
	}

	private static string TriggerJson(JsonElement trigger, string chunk)
	{
		using var stream = new MemoryStream();
		using (var w = new Utf8JsonWriter(stream))
		{
			w.WriteStartObject();
			foreach (JsonProperty p in trigger.EnumerateObject())
			{
				if (p.Name is "center" or "extents")
				{
					continue; // the marker entity's transform is the live truth
				}
				w.WritePropertyName(p.Name);
				p.Value.WriteTo(w);
			}
			w.WriteString("chunk", chunk);
			w.WriteEndObject();
		}
		return System.Text.Encoding.UTF8.GetString(stream.ToArray());
	}

	private static void WriteVec(Utf8JsonWriter w, Vector3 v)
	{
		w.WriteStartArray();
		w.WriteNumberValue(v.X);
		w.WriteNumberValue(v.Y);
		w.WriteNumberValue(v.Z);
		w.WriteEndArray();
	}

	private static Entity MarkerEntity(Entity root, string name, Vector3 position)
	{
		Entity e = World.Create();
		e.Name = name;
		e.AddTransform();
		e.SetParent(root);
		e.Position = position;
		return e;
	}

	private static Entity SpawnModel(Entity root, string name, string path, Vector3 position, Vector3 euler)
	{
		Entity e = World.Create();
		e.Name = name;
		e.AddTransform();
		e.SetParent(root);
		e.Position = position;
		e.EulerDegrees = euler;
		e.LoadModel(path);
		return e;
	}

	private static void Mark(Entity e, int role, string identity)
	{
		ComponentAccess marker = e.Component("Twinsanity Marker");
		marker.Add();
		marker.SetInt("role", role);
		marker.SetString("identity", identity);
	}
}
