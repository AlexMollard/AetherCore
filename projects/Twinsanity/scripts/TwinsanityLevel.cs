using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Builds a Twinsanity chunk from tw-extract's levels/&lt;chunk&gt;.level.json at play time - scenery,
/// collision, crates, wumpa, Crash's spawn - and runs its rules: breaking crates, collecting
/// wumpa, lives, checkpoints and dying. Nothing ISO-derived is in the scene file itself, so the
/// scene is committable and the content stays in the gitignored assets folder.
///
/// Object IDs are DefaultEnums.ObjectID from the Twinsanity editor. Crates are 1 unit cubes with
/// their origin at the bottom centre; wumpa sit about 1 unit above their origin.
/// </summary>
public sealed class TwinsanityLevel : EntityScript, TwinsanityActors.ITwinsanityHost
{
	public string LevelPath = "project://assets/levels/Earth/Hub/beach.level.json";
	public string ObjectsPath = "project://assets/levels/objects.json";
	public string PlayerName = "Crash";
	public float KillY = -35.0f;
	public int StartLives = 4;
	// Play the New Game movie (FMV/H01_A, tw-extract --movies H01_A) before the beach starts.
	public bool IntroMovie = false;
	public float SkyScale = 7.5f;

	private const float kCrashRadius = 0.4f;
	private const float kCrashHeight = 1.8f;
	private const float kExplosionRadius = 2.5f;

	// Crate kinds; internal so TwinsanityBake classifies with the same table.
	internal enum Kind { Basic, Nitro, Tnt, ExtraLife, WoodenSpring, IronSpring, Iron, Checkpoint, AkuAku, MultiHit, Level, Surprise, Detonator, Reinforced }

	/// <summary>The crate kind for a level instance, or null when it is not a crate the level
	/// rules handle (it then belongs to the actor system). Shared with the bake.</summary>
	internal static Kind? KindFor(int objectId, string? model)
	{
		Kind? kind = objectId switch
		{
			3 => Kind.Basic,
			4 => Kind.Nitro,
			5 => Kind.Tnt,
			12 => Kind.ExtraLife,
			13 => Kind.WoodenSpring,
			14 => Kind.IronSpring,
			15 => Kind.Iron,
			19 => Kind.MultiHit,
			266 => Kind.Checkpoint,
			297 => Kind.AkuAku,
			_ => null,
		};
		return kind ?? NameKind(model);
	}

	private sealed class Crate
	{
		public Kind Kind;
		public Entity Body;
		public Entity Model;
		public Vector3 Base;
		public bool Alive = true;
		public float Fuse = -1.0f;
		public int Hits;                       // remaining hits for MultiHit crates
		public int ObjectId;                   // the original data's object id (CrateFx keys on it)
		public bool Activated;                 // checkpoint crates open (keep their model) instead of breaking
		public bool CheckSupport;              // the crate under it broke or moved: see if it must fall
		public bool Falling;
		public float FallSpeed;
		// Baked crates: the body is a CHILD of the model (one selectable unit in the editor), so
		// it follows the model's transform and must not be moved again by the fall code.
		public bool BodyIsChild;
	}

	private readonly List<Crate> _crates = new();
	private TwinsanityWumpa _fruit = new(_ => { }); // re-created in OnAttach with AddWumpa
	private readonly TwinsanityActors _actors = new();
	private readonly TwinsanityHud _hud = new();
	private readonly TwinsanityPause _pause = new();
	private readonly TwinsanityCutscenes _cutscenes = new();
	private readonly TwinsanityMovie _movie = new();
	private readonly HashSet<uint> _deadly = new();
	private readonly Dictionary<int, string> _objectModels = new();

	private Entity _crash;
	private CrashPlayer? _player;
	private Vector3 _spawn;
	private float[]? _crashFloats;
	private float _spawnFacing;
	private Vector3 _checkpoint;
	private float _checkpointFacing;
	private bool _placed;

	private int _wumpaCount;
	private int _lives;
	// Aku Aku masks, 0-2. The rig's health word is 1 + masks: a fresh beach reads 2 (one mask), a
	// crab takes 3 -> 2 -> 1 -> dead, and the respawn reads 2 (logs/aku/track_hitcrab.csv).
	private int _aku = 1;
	private float _deathTimer = -1.0f;
	// The original's grace after a hit (the 2 s hurt flicker): no second hit lands inside it
	// (rig logs/gameplay/rig_crab_notice.csv: a crab in contact took masks at 17.0 s and 19.0 s).
	private const float HurtGrace = 2.0f;
	private float _hurtGrace;
	private int _gems; // collected gems, one bit per TwinsanityPause.GemSlot
	private Entity _sky;

	public override void OnAttach()
	{
		_lives = StartLives;
		_fruit = new TwinsanityWumpa(AddWumpa);
		LoadObjectModels();

		// A baked level (the editor's Twinsanity bake, saved with the scene as a prefab
		// instance) is bound to, not rebuilt: every entity already exists and carries its
		// disc identity in a Twinsanity Marker. Only when there is no bake at all - a fresh
		// checkout - does the level build from the extracted JSON. A bake root whose marker
		// cannot be read (an editor binary older than the marker component) must not fall
		// back: the baked entities are already in the scene and a JSON build would stack a
		// second copy of the whole level on top of them.
		Entity bakeRoot = Scene.Find(TwinsanityBake.BakeRootName);
		if (!bakeRoot.IsValid)
		{
			BuildFromJson();
		}
		else if (bakeRoot.Component("Twinsanity Marker").Exists)
		{
			BindBaked(bakeRoot);
		}
		else
		{
			Log.Error("[Twinsanity] the baked level has no readable Twinsanity Marker (editor build predates the bake?) - rebuild the editor; not building the level a second time.");
		}

		CrateFx.RegisterObjectModels(_objectModels);
		OpenStartCheckpoint();
		Log.Info($"[Twinsanity] {_crates.Count} crates, {_fruit.Count} wumpa, {_deadly.Count} deadly collision pieces");
		if (!IntroMovie || !_movie.Play("H01_A", () => TwinsanityAudio.Start(LevelPath)))
		{
			TwinsanityAudio.Start(LevelPath);
		}
		TwinsanityAku.Start();
		_cutscenes.Start();
		// The HUD (wumpa and lives counters, pause menu) is TwinsanityHud, fed from OnUpdate; its
		// summary has the rig evidence for when the original shows it.
	}

	private void LoadObjectModels()
	{
		string? objects = Assets.ReadText(ObjectsPath);
		if (objects != null)
		{
			using JsonDocument table = JsonDocument.Parse(objects);
			foreach (JsonProperty row in table.RootElement.EnumerateObject())
			{
				if (row.Value.TryGetProperty("model", out JsonElement model))
				{
					_objectModels[int.Parse(row.Name)] = model.GetString()!;
				}
			}
		}
		else
		{
			Log.Warn($"[Twinsanity] {ObjectsPath} missing - run tw-extract over the whole disc; crates and wumpa from other files will be invisible.");
		}
	}

	// The play-time build: the start chunk plus every chunk reachable over its chunk links
	// (BFS, each chunk once). A link's transform is relative to its own chunk, so a child's
	// world transform is local * parent (row vectors). Link targets outside the extracted set
	// are skipped. Only seamless-neighbour links (flags low byte 1) in the start chunk's own
	// level folder stream in; kind 2 is a door into another space (Doc's lab, the totem, level
	// entrances) and kind 0 the boat trip - their transforms do not agree with the hub's loops.
	private void BuildFromJson()
	{
		string levelFolder = LevelPath[..(LevelPath.LastIndexOf('/') + 1)];
		var queue = new Queue<(string Path, Matrix4x4 Transform)>();
		var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		queue.Enqueue((LevelPath, Matrix4x4.Identity));
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
				if (path == LevelPath)
				{
					Log.Error($"[Twinsanity] {path} not found - run tools/tw-extract (see docs/twinsanity-editor.md).");
				}
				else
				{
					Log.Warn($"[Twinsanity] linked chunk {path} not extracted - skipping.");
				}
				continue;
			}
			using JsonDocument doc = JsonDocument.Parse(text);
			LoadChunk(doc.RootElement, transform, path == LevelPath);
			_cutscenes.AddChunk(path, doc.RootElement, transform);
			if (doc.RootElement.TryGetProperty("links", out JsonElement links))
			{
				foreach (JsonElement link in links.EnumerateArray())
				{
					string chunk = link.GetProperty("chunk").GetString()!;
					bool neighbour = link.TryGetProperty("flags", out JsonElement flags) && (flags.GetUInt32() & 0xFF) == 1;
					if (neighbour && chunk.StartsWith(levelFolder, StringComparison.OrdinalIgnoreCase))
					{
						queue.Enqueue((chunk, ChunkTransform(link) * transform));
					}
				}
			}
		}
	}

	// The baked path: adopt every marker entity into the same runtime state BuildFromJson fills
	// - crates, wumpa, actors, deadly collision, spawn, sky - and hand the cutscene agents and
	// triggers to the cutscene system grouped by their chunk (each chunk's scripts.json loads
	// once). The scan is scene-wide, not just the bake root's subtree: a hand-placed COPY of a
	// baked entity (editor duplicate) re-roots outside it, and must behave identically. The
	// JSON paths are never read here; the markers carry the world-space instance data.
	private void BindBaked(Entity bakeRoot)
	{
		// A generous fixed buffer: the beach runs ~3.5k entities with transforms; the native
		// side clamps to the capacity and returns the written count.
		var buffer = new Entity[16384];
		int written = World.GetEntitiesWithTransform(buffer);
		var entities = new List<Entity>(Math.Max(0, written));
		for (int i = 0; i < written; i++)
		{
			entities.Add(buffer[i]);
		}
		var agentsByChunk = new SortedDictionary<string, List<JsonElement>>();
		var triggersByChunk = new SortedDictionary<string, List<(JsonElement Json, Vector3 Center, Vector3 Extents)>>();
		int actors = 0, spawners = 0, cutsceneAgents = 0;
		var openDocs = new List<JsonDocument>();
		foreach (Entity e in entities)
		{
			ComponentAccess marker = e.Component("Twinsanity Marker");
			if (!marker.Exists)
			{
				continue;
			}
			int role = marker.GetInt("role");
			string identity = marker.GetString("identity");
			if (role == TwinsanityBake.RoleRoot || identity.Length == 0)
			{
				continue;
			}
			JsonDocument doc;
			try
			{
				doc = JsonDocument.Parse(identity);
			}
			catch (Exception ex)
			{
				// A hand-edited or stale marker must cost itself, not the whole bind.
				Log.Warn($"[Twinsanity] bad marker JSON on '{e.Name}' - skipped ({ex.Message})");
				continue;
			}
			openDocs.Add(doc);
			JsonElement json = doc.RootElement;
			try
			{
				switch (role)
				{
				case TwinsanityBake.RoleCrate:
				{
					string kindName = json.TryGetProperty("crate", out JsonElement ck) ? ck.GetString()! : Kind.Basic.ToString();
					Kind kind = Enum.TryParse(kindName, out Kind parsed) ? parsed : Kind.Basic;
					Entity body = default;
					for (int i = 0; i < e.ChildCount; i++)
					{
						Entity child = e.GetChild(i);
						if (child.Component("Rigid Body").Exists)
						{
							body = child;
							break;
						}
					}
					_crates.Add(new Crate
					{
						Kind = kind,
						Body = body,
						Model = e,
						Base = e.Position,
						Hits = kind == Kind.MultiHit ? 3 : 1,
						ObjectId = json.TryGetProperty("objectId", out JsonElement oi) ? oi.GetInt32() : 0,
						BodyIsChild = true,
					});
					break;
				}
				case TwinsanityBake.RoleWumpa:
					_fruit.Bind(e, e.Position);
					break;
				case TwinsanityBake.RoleDeadlyCollision:
					_deadly.Add(e.Id);
					break;
				case TwinsanityBake.RoleSpawn:
					_spawn = e.Position;
					_spawnFacing = json.TryGetProperty("facing", out JsonElement f) ? f.GetSingle() : e.EulerDegrees.Y;
					if (json.TryGetProperty("floats", out JsonElement floats) && floats.ValueKind == JsonValueKind.Array)
					{
						var list = new List<float>();
						foreach (JsonElement v in floats.EnumerateArray())
						{
							list.Add(v.GetSingle());
						}
						_crashFloats = list.ToArray();
					}
					_checkpoint = _spawn;
					_checkpointFacing = _spawnFacing;
					break;
				case TwinsanityBake.RoleSky:
					_sky = e;
					break;
				case TwinsanityBake.RoleCutsceneAgent:
				{
					string chunk = json.TryGetProperty("chunk", out JsonElement c) ? c.GetString()! : "";
					if (chunk.Length > 0)
					{
						(agentsByChunk.TryGetValue(chunk, out List<JsonElement>? list) ? list : agentsByChunk[chunk] = new List<JsonElement>()).Add(json);
						cutsceneAgents++;
					}
					break;
				}
				case TwinsanityBake.RoleTrigger:
				{
					string chunk = json.TryGetProperty("chunk", out JsonElement c) ? c.GetString()! : "";
					if (chunk.Length > 0)
					{
						if (!triggersByChunk.TryGetValue(chunk, out var list))
						{
							list = triggersByChunk[chunk] = new List<(JsonElement, Vector3, Vector3)>();
						}
						list.Add((json, e.Position, e.Scale));
					}
					break;
				}
				default:
					// Actors, creature spawners and parrot spawners: the actor system adopts them.
					if (_actors.TryBind(e, role, json))
					{
						if (role is TwinsanityBake.RoleSpawner or TwinsanityBake.RoleParrotSpawner)
						{
							spawners++;
						}
						else
						{
							actors++;
						}
					}
					break;
				}
			}
			catch (Exception ex)
			{
				Log.Warn($"[Twinsanity] marker bind failed on '{e.Name}' (role {role}) - skipped ({ex.Message})");
			}
		}
		// Every chunk that has agents OR triggers: a chunk whose agent markers were all
		// skipped still owns its trigger volumes, and dropping them strands the scenes
		// those volumes start (the start chunk's cutscenes, before the bake named it).
		var chunks = new SortedSet<string>(agentsByChunk.Keys, StringComparer.Ordinal);
		chunks.UnionWith(triggersByChunk.Keys);
		foreach (string chunkPath in chunks)
		{
			_cutscenes.BindChunkScripts(chunkPath);
			if (agentsByChunk.TryGetValue(chunkPath, out List<JsonElement>? agents))
			{
				foreach (JsonElement agent in agents)
				{
					_cutscenes.BindAgent(agent);
				}
			}
			if (triggersByChunk.TryGetValue(chunkPath, out var triggers))
			{
				foreach ((JsonElement json, Vector3 center, Vector3 extents) in triggers)
				{
					_cutscenes.BindTrigger(json, center, extents);
				}
			}
		}
		foreach (JsonDocument doc in openDocs)
		{
			doc.Dispose();
		}
		Log.Info($"[Twinsanity] bound baked level: {actors} actors, {spawners} spawners, {cutsceneAgents} cutscene agents");
	}

	public override void OnUpdate(float deltaTime)
	{
		_movie.Update();
		Entity camera = Camera.Main;
		if (_sky.IsValid && camera.IsValid)
		{
			_sky.Position = camera.Position;
		}
		if (!FindPlayer())
		{
			return;
		}
		if (!_placed)
		{
			_placed = true;
			if (_crashFloats != null)
			{
				_player!.Configure(_crashFloats);
			}
			_player!.Respawn(_spawn, _spawnFacing);
			return;
		}

		_fruit.Update(deltaTime, _crash.Position);
		_hurtGrace -= deltaTime;
		TwinsanityAudio.Update(deltaTime, _player!);
		UpdateFuses(deltaTime);
		UpdateStacks(deltaTime);
		DebugWarpPoll(deltaTime);
		// Keep world life and crate fx animating through the death pause, as in the original.
		_actors.Update(deltaTime, _player!, this);
		CrateFx.Update(deltaTime);
		_cutscenes.Update(deltaTime, _player!);
		TwinsanityAku.Update(deltaTime, _player!, _aku);
		_hud.Update(_wumpaCount, _lives, _deathTimer >= 0.0f);
		// The pause menu ticks on the unscaled clock: it freezes the game itself (Time.Scale 0)
		// while its own opening, drum and closing keep animating, as in the original.
		_pause.Update(_wumpaCount, _lives, _gems);
		if (_pause.JustClosed)
		{
			_hud.PopBoth(); // the HUD pops back in when the menu closes (rig_pause_slow.png)
		}

		if (_deathTimer >= 0.0f)
		{
			_deathTimer -= deltaTime;
			if (_deathTimer < 0.0f)
			{
				_player!.Respawn(_checkpoint + new Vector3(0.0f, 0.1f, 0.0f), _checkpointFacing);
				_player.SetControl(true);
				_aku = 1;
				Log.Info("[Twinsanity] Crash respawned at checkpoint");
			}
			return;
		}

		Vector3 feet = _crash.Position;
		TouchCrates(feet);
		if (feet.Y < KillY)
		{
			Die(DeathKind.Fall); // fell off the world
		}
		else if (OnDeadlyGround(feet, out Vector3 water))
		{
			// What Crash stands on here is the drowning plane (surface 23); the water surface
			// (surface 12) has no collider. In the hub collision every sea's surface sits 1.8
			// above its drowning plane (beach, huba, hubb, hubc, hubd: -1.5 over -3.3; pier
			// -216.84 over -218.64). His feet are on the plane: a ray down through him hits his
			// own inner body first (feet + 0.5, which floated him 0.5 above the rig's -1.5).
			// ponytail: a few inland pools differ (1.46 to 2.4 over their plane); export the
			// surface-12 heights per piece if those ever need the exact float height.
			_player!.DrownSurfaceY = water.Y + WaterAboveDrownPlane;
			Die(DeathKind.Drown); // every deadly piece in the hub is the sea
		}
	}

	private const float WaterAboveDrownPlane = 1.8f;

	private bool FindPlayer()
	{
		if (_player != null)
		{
			return true;
		}
		_crash = Scene.Find(PlayerName);
		_player = _crash.IsValid ? _crash.GetScript<CrashPlayer>() : null;
		return _player != null;
	}

	private void LoadChunk(JsonElement root, Matrix4x4 transform, bool start)
	{
		string name = root.GetProperty("level").GetString()!;
		foreach (JsonElement path in root.GetProperty("scenery").EnumerateArray())
		{
			// Listed by naming convention; not every chunk has dynamic scenery.
			if (Assets.List(path.GetString()!).Length > 0)
			{
				Spawn("Scenery", path.GetString()!, transform);
			}
		}
		if (start)
		{
			// Twinsanity draws its skydome around the camera, behind everything. The extracted dome is
			// a 120-unit sphere at the chunk origin, so left in place it swallows any scenery further out.
			// Kept on the camera and grown to just inside the far plane (1000), it stays behind the world.
			_sky = Spawn("Sky", root.GetProperty("sky").GetString()!, Matrix4x4.Identity);
			_sky.Scale = new Vector3(SkyScale);
			for (int i = 0; i < _sky.ChildCount; i++)
			{
				MeshRenderer.SetCastShadows(_sky.GetChild(i), false);
			}
		}
		foreach (JsonElement piece in root.GetProperty("collision").EnumerateArray())
		{
			Entity e = World.Create();
			e.Name = "Collision";
			e.AddTransform();
			e.Position = transform.Translation;
			e.EulerDegrees = EulerOf(transform);
			Physics.AddMeshBody(e, piece.GetProperty("path").GetString()!);
			if (piece.GetProperty("deadly").GetBoolean())
			{
				_deadly.Add(e.Id);
			}
		}
		if (start && root.TryGetProperty("spawn", out JsonElement spawn))
		{
			_spawn = Vector3.Transform(Vec(spawn.GetProperty("position")), transform);
			// The instance yaw turns a +Z-facing model; CrashPlayer's facing is the camera yaw.
			_spawnFacing = EulerOf(SysRotation(Vec(spawn.GetProperty("euler"))) * transform).Y + 180.0f;
			if (spawn.TryGetProperty("floats", out JsonElement floats))
			{
				_crashFloats = floats.EnumerateArray().Select(f => f.GetSingle()).ToArray();
			}
			_checkpoint = _spawn;
			_checkpointFacing = _spawnFacing;
		}
		foreach (JsonElement instance in root.GetProperty("instances").EnumerateArray())
		{
			AddInstance(instance, transform);
		}
		Log.Info($"[Twinsanity] chunk {name}: {_crates.Count} crates, {_fruit.Count} wumpa, {_deadly.Count} deadly pieces so far");
	}

	// On the rig a fresh beach load already shows the level-start checkpoint open (flat pieces, no crate
	// to hit; logs/camera/_rig_cp.png), and a death before any other checkpoint respawns him standing on
	// those pieces (game (-1.07, 0.07, -39.41); the crate is at engine (1.07, -39.41)). So it opens at load,
	// silently, and is the first checkpoint rather than the spawn. He respawns facing the crate's own
	// yaw: the rig runs off at heading 140.9 after a respawn, the crate's instance yaw is 140.87.
	// ponytail: "nearest checkpoint to the spawn" stands in for the level's own start-checkpoint link.
	private void OpenStartCheckpoint()
	{
		Crate? first = _crates.Where(c => c.Kind == Kind.Checkpoint && c.Alive).MinBy(c => Vector3.DistanceSquared(c.Base, _spawn));
		if (first == null)
		{
			return;
		}
		first.Activated = true;
		first.Alive = false;
		first.Body.Destroy();
		CrateFx.ShowOpened(first.Model, first.ObjectId);
		_checkpoint = first.Base;
		_checkpointFacing = CheckpointFacing(first);
	}

	// CrashPlayer's facing is the camera yaw: the instance yaw turns a +Z-facing model (as the spawn).
	private static float CheckpointFacing(Crate c) => c.Model.EulerDegrees.Y + 180.0f;

	// A level.json link entry -> the chunk's local transform (rotation, then offset), in
	// System.Numerics row-vector form. Internal: TwinsanityBake walks the same links.
	internal static Matrix4x4 ChunkTransform(JsonElement link)
	{
		Matrix4x4 m = Matrix4x4.CreateTranslation(Vec(link.GetProperty("offset")));
		if (link.TryGetProperty("rotation", out JsonElement r))
		{
			var q = new Quaternion(r[0].GetSingle(), r[1].GetSingle(), r[2].GetSingle(), r[3].GetSingle());
			m = Matrix4x4.CreateFromQuaternion(q) * m;
		}
		return m;
	}

	// The engine composes EulerDegrees as R = Ry * Rx * Rz (column vectors); System.Numerics works
	// with row vectors, so a rotation crosses between the two as the transpose. EulerOf takes a
	// row-vector transform and returns the engine's Euler degrees of its rotation.
	// (Internal: the bake transforms instance transforms with the same pair.)
	internal static Vector3 EulerOf(Matrix4x4 sys)
	{
		const float deg = 180.0f / MathF.PI;
		float x = MathF.Asin(Math.Clamp(-sys.M32, -1.0f, 1.0f));
		float y = MathF.Atan2(sys.M31, sys.M33);
		float z = MathF.Atan2(sys.M12, sys.M22);
		return new Vector3(x, y, z) * deg;
	}

	// Euler degrees (engine convention) -> row-vector rotation matrix: the transpose of Ry * Rx * Rz.
	internal static Matrix4x4 SysRotation(Vector3 euler)
	{
		float x = euler.X * MathF.PI / 180.0f, y = euler.Y * MathF.PI / 180.0f, z = euler.Z * MathF.PI / 180.0f;
		return Matrix4x4.CreateRotationZ(z) * Matrix4x4.CreateRotationX(x) * Matrix4x4.CreateRotationY(y);
	}

	private void AddInstance(JsonElement instance, Matrix4x4 transform)
	{
		int objectId = instance.GetProperty("object").GetInt32();
		Vector3 position = Vector3.Transform(Vec(instance.GetProperty("position")), transform);
		Vector3 euler = EulerOf(SysRotation(Vec(instance.GetProperty("euler"))) * transform);
		string? model = instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : _objectModels.GetValueOrDefault(objectId);

		if (objectId == 1)
		{
			if (model != null)
			{
				_fruit.Spawn(model, position, euler.Y);
			}
			return;
		}
		Kind? kind = KindFor(objectId, model);
		if (kind == null)
		{
			// Not a crate: hand it to the actor system (enemies, birds, butterflies, chickens...).
			string objectName = TwinsanityActors.InstanceName(instance, model);
			float[] floats = instance.TryGetProperty("floats", out JsonElement fl)
				? fl.EnumerateArray().Select(f => f.GetSingle()).ToArray()
				: Array.Empty<float>();
			uint subtype = instance.TryGetProperty("subtype", out JsonElement st) ? st.GetUInt32() : 0u;
			_actors.TrySpawn(objectId, objectName, model, position, euler, floats, subtype, instance, transform);
			return;
		}
		if (model == null)
		{
			return;
		}
		// shortcut: the box collider and the touch tests below are axis-aligned; crates placed at
		// a yaw other than a multiple of 90 degrees get a slightly wrong footprint.
		Entity body = World.Create();
		body.Name = kind.ToString() + " Crate";
		body.AddTransform();
		body.Position = position + new Vector3(0.0f, 0.5f, 0.0f);
		Physics.AddBoxBody(body, new Vector3(0.5f, 0.5f, 0.5f), dynamic: false);
		Entity crateModel = Spawn(body.Name, model, position, euler);
		CrateFx.Spawned(crateModel, objectId, model);
		_crates.Add(new Crate { Kind = kind.Value, Body = body, Model = crateModel, Base = position, Hits = kind.Value == Kind.MultiHit ? 3 : 1, ObjectId = objectId });
	}

	// Some crate kinds are only distinguishable by model name (their object ids differ per chunk
	// file in the original data).
	private static Kind? NameKind(string? model)
	{
		if (model == null)
		{
			return null;
		}
		string name = model.ToUpperInvariant();
		if (name.Contains("CRATE"))
		{
			if (name.Contains("SURPRISE")) return Kind.Surprise;             // the "?" crate
			if (name.Contains("DETONATOR")) return Kind.Detonator;           // TNT detonator switch
			if (name.Contains("MULTIPLEHIT")) return Kind.MultiHit;          // needs several hits
			if (name.Contains("REINFORCED")) return Kind.Reinforced;         // metal-clad but breakable (REINFORCED_WOODEN_CRATE_BREAK = 654)
			if (name.Contains("INVISIBLE_CHECKPOINT")) return Kind.Checkpoint;
			if (name.Contains("LEVELCRATE")) return Kind.Level;              // the level-entrance crate
		}
		return null;
	}

	private static Entity Spawn(string name, string path, Matrix4x4 transform)
	{
		Entity e = Spawn(name, path, Vector3.Zero, Vector3.Zero);
		e.Position = transform.Translation;
		e.EulerDegrees = EulerOf(transform);
		return e;
	}

	private static Entity Spawn(string name, string path, Vector3 position, Vector3 euler)
	{
		Entity e = World.Create();
		e.Name = name;
		e.AddTransform();
		e.Position = position;
		e.EulerDegrees = euler;
		e.LoadModel(path);
		return e;
	}

	private float _warpPoll;

	// ponytail: dev-only test hook - polls project://warp.txt (shared by every playing editor), or
	// instead project://warp-<AETHER_CONTROL_PORT>.txt when that file exists (this editor only), and
	// teleports Crash there ("x y z", any other content = idle). Inert in normal play; remove when
	// automated testing gets a proper driver API.
	private static readonly string? kOwnWarp = Environment.GetEnvironmentVariable("AETHER_CONTROL_PORT") is { Length: > 0 } port
		? $"project://warp-{port}.txt" : null;

	private void DebugWarpPoll(float deltaTime)
	{
		_warpPoll -= deltaTime;
		if (_warpPoll > 0.0f)
		{
			return;
		}
		_warpPoll = 0.25f;
		string? text = (kOwnWarp != null ? Assets.ReadText(kOwnWarp) : null) ?? Assets.ReadText("project://warp.txt");
		if (text == null)
		{
			return;
		}
		string[] parts = text.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
		if (parts.Length == 3 && float.TryParse(parts[0], out float x) && float.TryParse(parts[1], out float y) && float.TryParse(parts[2], out float z))
		{
			_player!.Respawn(new Vector3(x, y, z), _player.Facing);
			Log.Info($"[Twinsanity] debug warp to ({x}, {y}, {z})");
		}
	}

	public void AddWumpa(int count)
	{
		_wumpaCount += count;
		TwinsanityAudio.Wumpa();
		while (_wumpaCount >= 100)
		{
			_wumpaCount -= 100;
			_lives++;
			TwinsanityAudio.ExtraLife();
		}
	}

	private void TouchCrates(Vector3 feet)
	{
		Vector3 velocity = _player!.Velocity;
		// Both the spin and the slide sweep break wooden crates in the original.
		bool whirled = _player.IsSpinning || _player.IsSliding;
		// Headbutt (OnHeadbutt in the original's behaviour tables): a jump stopped by the underside
		// of a crate hits that crate like a spin does. The lowest crate above Crash's footprint is
		// the one his head met.
		Crate? bonked = null;
		if (_player.ConsumeCeilingHit())
		{
			foreach (Crate c in _crates)
			{
				if (c.Alive && c.Base.Y > feet.Y && c.Base.Y - feet.Y < kCrashHeight + 1.0f
				    && MathF.Abs(feet.X - c.Base.X) < 0.5f + kCrashRadius && MathF.Abs(feet.Z - c.Base.Z) < 0.5f + kCrashRadius
				    && (bonked == null || c.Base.Y < bonked.Base.Y))
				{
					bonked = c;
				}
			}
		}
		foreach (Crate c in _crates.ToArray())
		{
			if (!c.Alive)
			{
				continue;
			}
			float dx = MathF.Abs(feet.X - c.Base.X);
			float dz = MathF.Abs(feet.Z - c.Base.Z);
			float top = c.Base.Y + 1.0f;
			bool overlapsVertically = feet.Y < top + 0.1f && feet.Y + kCrashHeight > c.Base.Y;

			// Standing on or landing on the lid. The character controller's feet rest about half a
			// unit below the collider top, so the band reaches further down than it looks like it
			// should.
			bool onTop = dx < 0.5f + kCrashRadius * 0.75f && dz < 0.5f + kCrashRadius * 0.75f
			             && feet.Y > top - 0.75f && feet.Y < top + 0.45f && velocity.Y < 0.5f;
			bool touching = dx < 0.5f + kCrashRadius + 0.08f && dz < 0.5f + kCrashRadius + 0.08f && overlapsVertically;
			bool whirledHit = (whirled && dx < 1.5f && dz < 1.5f && feet.Y < top + 0.5f && feet.Y + kCrashHeight > c.Base.Y) || c == bonked;

			switch (c.Kind)
			{
				case Kind.Nitro:
					// Nitro goes up on any contact, including a landing from above or a spin.
					if (onTop || touching || whirledHit)
					{
						Explode(c);
					}
					break;
				case Kind.Tnt:
					// Jumping on top starts the fuse and bounces Crash; spins and slides do nothing.
					if (onTop)
					{
						_player.Bounce(9.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
						if (c.Fuse < 0.0f)
						{
							c.Fuse = 2.2f; // rig: boom 2.2 s after the landing bounce (tnt_fuse_sheet)
						}
					}
					break;
				case Kind.Basic:
					// The original's single wooden crate pops the moment it is touched from above,
					// giving Crash a small bounce as it breaks.
					if (onTop)
					{
						Break(c);
						_player.Bounce(9.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
					}
					else if (whirledHit)
					{
						Break(c);
					}
					break;
				case Kind.MultiHit:
					if (onTop || whirledHit)
					{
						c.Hits--;
						_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, 5); // a handful of wumpa per hit, as in the original
						if (c.Hits <= 0)
						{
							Break(c);
						}
						if (onTop)
						{
							_player.Bounce(9.0f);
							CrateFx.Bounced(c.Model, c.ObjectId);
						}
					}
					break;
				case Kind.Surprise:
					if (onTop)
					{
						Break(c);
						_player.Bounce(9.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
					}
					else if (whirledHit)
					{
						Break(c);
					}
					break;
				case Kind.Level:
					// Smashing a level crate is the hub's door into that level; entering the level
					// itself is out of scope, so the crate simply breaks.
					if (onTop)
					{
						Break(c);
						_player.Bounce(9.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
						Log.Info("[Twinsanity] Level crate smashed (level entry not implemented).");
					}
					else if (whirledHit)
					{
						Break(c);
					}
					break;
				case Kind.Checkpoint:
					// The checkpoint breaks open on the first hit (the OGI flips to the opened look,
					// 245/246): the bounce still happens, then its collider goes so Crash can walk
					// through the opened crate and never lands on it again.
					if (onTop || whirledHit)
					{
						c.Activated = true;
						_checkpoint = c.Base;
						_checkpointFacing = CheckpointFacing(c);
						Log.Info("[Twinsanity] Checkpoint activated.");
						CrateFx.Activated(c.Model, c.ObjectId);
						if (onTop)
						{
							_player.Bounce(9.0f);
							CrateFx.Bounced(c.Model, c.ObjectId);
						}
						c.Alive = false;
						c.Body.Destroy();
					}
					break;
				case Kind.Detonator:
					// DETONATOR_CRATE_SPUN (4790) in the engine's state table: a spin sets it off;
					// in practice any break does, and it lights every TNT in the level.
					if (onTop || whirledHit)
					{
						Break(c);
						foreach (Crate tnt in _crates)
						{
							if (tnt.Alive && tnt.Kind == Kind.Tnt && tnt.Fuse < 0.0f)
							{
								tnt.Fuse = 0.3f;
							}
						}
					}
					break;
				case Kind.Reinforced:
					// REINFORCED_WOODEN_CRATE_BREAK (654): breakable like a wooden crate.
					if (onTop)
					{
						Break(c);
						_player.Bounce(9.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
					}
					else if (whirledHit)
					{
						Break(c);
					}
					break;
				case Kind.Iron:
					break;
				case Kind.IronSpring:
					if (onTop)
					{
						_player.Bounce(16.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
					}
					break;
				case Kind.WoodenSpring:
					if (whirledHit)
					{
						Break(c);
					}
					else if (onTop)
					{
						_player.Bounce(16.0f);
						CrateFx.Bounced(c.Model, c.ObjectId);
					}
					break;
				default:
					if (whirledHit || onTop)
					{
						Break(c);
					}
					break;
			}
			if (_deathTimer >= 0.0f)
			{
				return;
			}
		}
	}

	private void Break(Crate c)
	{
		if (!c.Alive)
		{
			return;
		}
		c.Alive = false;
		c.Body.Destroy();
		CrateFx.Broken(c.Model, c.ObjectId); // plays the fragment clip, then destroys the model
		QueueAbove(c);
		switch (c.Kind)
		{
			case Kind.Basic:
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, 5);
				break;
			case Kind.Surprise: // the "?" crate bursts into wumpa
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, 5);
				break;
			case Kind.ExtraLife:
				_lives++;
				TwinsanityAudio.ExtraLife();
				break;
			case Kind.AkuAku:
				_aku = TwinsanityAku.Collect(_aku);
				break;
		}
	}

	// Stacked crates. Rig (logs/wildlife/stack_fall_rig_raw.csv, the iron crates on the nitro stack
	// at (24.27, -7.9)): once the crate under one is gone it drops straight down, from rest, at
	// 0.0198 m/frame^2 at 50 fps (49.5 m/s^2), no bounce, and lands flush on the next support
	// (3.01 -> 0.0 on the ground, 4.01 -> 1.0 on the crate below). The crate above a falling one
	// starts 7 frames later: it lets go once its supporter's lid has dropped more than 0.7 m.
	private const float kStackGravity = 49.5f;
	private const float kStackLetGo = 0.7f;

	private void QueueAbove(Crate below)
	{
		foreach (Crate c in _crates)
		{
			if (c.Alive && c != below && !c.Falling && OverFootprint(c, below)
				&& c.Base.Y > below.Base.Y + 0.5f && c.Base.Y < below.Base.Y + 1.5f)
			{
				c.CheckSupport = true;
			}
		}
	}

	private static bool OverFootprint(Crate a, Crate b) =>
		MathF.Abs(a.Base.X - b.Base.X) < 0.5f && MathF.Abs(a.Base.Z - b.Base.Z) < 0.5f;

	// Highest lid under c among live crates (or float.MinValue).
	private float CrateSupportTop(Crate c)
	{
		float top = float.MinValue;
		foreach (Crate s in _crates)
		{
			if (s.Alive && s != c && s.Base.Y < c.Base.Y && OverFootprint(c, s))
			{
				top = MathF.Max(top, s.Base.Y + 1.0f);
			}
		}
		return top;
	}

	private void UpdateStacks(float dt)
	{
		foreach (Crate c in _crates)
		{
			if (!c.Alive || !(c.CheckSupport || c.Falling))
			{
				continue;
			}
			float support = CrateSupportTop(c);
			if (!c.Falling)
			{
				if (support >= c.Base.Y - kStackLetGo)
				{
					// Still held; stop watching once the supporter has come to rest.
					c.CheckSupport = _crates.Exists(s => s.Falling && s.Alive && OverFootprint(c, s) && s.Base.Y < c.Base.Y);
					continue;
				}
				c.Falling = true;
				c.CheckSupport = false;
				c.FallSpeed = 0.0f;
				Physics.SetMotionType(c.Body, PhysicsMotionType.Kinematic);
				QueueAbove(c);
			}
			// The ground ray is for the terrain: crates below are the support above. A kinematic
			// crate body trails (or overshoots) its model by a physics step, so a hit on any
			// crate's body - its own included - is a stale lid that froze the fall mid-air.
			RaycastHit ground = Physics.Raycast(c.Base - new Vector3(0.0f, 0.005f, 0.0f), -Vector3.UnitY, 60.0f);
			bool terrain = ground.DidHit && !_crates.Exists(s => s.Body.Id == ground.Entity.Id);
			float floor = MathF.Max(support, terrain ? ground.Position.Y : float.MinValue);
			c.FallSpeed += kStackGravity * dt;
			float y = c.Base.Y - c.FallSpeed * dt;
			if (y <= floor)
			{
				y = floor;
				c.Falling = false;
				c.FallSpeed = 0.0f;
			}
			Vector3 move = new(0.0f, y - c.Base.Y, 0.0f);
			c.Base += move;
			if (!c.BodyIsChild)
			{
				c.Body.Position += move; // baked crates: the body is a child, it follows the model
			}
			c.Model.Position += move;
			CrateFx.Moved(c.Model, c.Base); // nitro hops snap back to their cached rest position
		}
	}

	private void Explode(Crate c)
	{
		if (!c.Alive)
		{
			return;
		}
		Break(c);
		CrateFx.Exploded(c.Base, c.ObjectId);
		Vector3 center = c.Base + new Vector3(0.0f, 0.5f, 0.0f);
		_actors.Explosion(center, kExplosionRadius);
		_actors.CreatureBlast(center, kExplosionRadius);
		Vector3 crashCenter = _crash.Position + new Vector3(0.0f, kCrashHeight * 0.5f, 0.0f);
		if (Vector3.Distance(center, crashCenter) < kExplosionRadius + kCrashRadius)
		{
			Hurt(center, DeathKind.Explode);
		}
		foreach (Crate other in _crates)
		{
			if (!other.Alive || Vector3.Distance(center, other.Base + new Vector3(0.0f, 0.5f, 0.0f)) > kExplosionRadius)
			{
				continue;
			}
			if (other.Kind is Kind.Nitro or Kind.Tnt)
			{
				other.Fuse = 0.05f; // chain on the next frames, not recursively
			}
			else if (other.Kind is not (Kind.Iron or Kind.IronSpring))
			{
				Break(other);
			}
		}
	}

	private void UpdateFuses(float deltaTime)
	{
		foreach (Crate c in _crates.ToArray())
		{
			if (c.Alive && c.Fuse >= 0.0f)
			{
				c.Fuse -= deltaTime;
				if (c.Fuse < 0.0f)
				{
					Explode(c);
				}
			}
		}
	}

	// TwinsanityActors.ITwinsanityHost: creatures brush nitro crates, as on the rig (chickens
	// walking into the coop detonate them).
	public void CreatureTouch(Vector3 position)
	{
		foreach (Crate c in _crates)
		{
			if (c.Alive && c.Kind == Kind.Nitro && Vector3.Distance(c.Base + new Vector3(0.0f, 0.5f, 0.0f), position) < 1.0f)
			{
				Explode(c);
			}
		}
	}

	private void Hurt(Vector3 from, DeathKind kind = DeathKind.Generic)
	{
		if (TwinsanityAku.Invincible)
		{
			return; // the mask on his face: nothing hurts him (rig logs/aku/track_l3.csv, _invwalk.png)
		}
		// Every explosion deals 100 damage and goes through the masks (wiki what-changed.md; rig
		// logs/gameplay/rig_blast1.csv: three masks, one nitro, dead in fire gibs, no knockback).
		if (kind == DeathKind.Explode)
		{
			Die(kind);
			return;
		}
		if (_hurtGrace > 0.0f)
		{
			return;
		}
		if (_aku > 0)
		{
			// Aku Aku eats the hit: the original's recoil and a shove away from the hazard.
			_aku--;
			_hurtGrace = HurtGrace;
			TwinsanityAudio.AkuLost();
			_player!.Hurt(from);
			return;
		}
		Die(kind);
	}

	// TwinsanityActors.ITwinsanityHost: a gem pickup fills its slot in the pause-menu gem track.
	public void CollectGem(int slot) => _gems |= 1 << slot;

	// TwinsanityActors.ITwinsanityHost: enemies and hazards funnel their hits through here.
	public void DamagePlayer(Vector3 from, DeathKind kind)
	{
		if (_deathTimer >= 0.0f)
		{
			return;
		}
		Hurt(from, kind);
	}

	private void Die(DeathKind kind)
	{
		if (_deathTimer >= 0.0f || _player == null)
		{
			return;
		}
		_lives--;
		if (_lives < 0)
		{
			// shortcut: the original's game over just reloads the level; we reset in place instead
			// of a title-screen round trip.
			_lives = StartLives;
			_wumpaCount = 0;
		}
		_aku = 0; // the respawn hands the first mask back
		Log.Info($"[Twinsanity] Crash died ({kind}), lives now {_lives}");
		_deathTimer = _player.Die(kind); // Die takes control and keeps the model visible
	}

	private bool OnDeadlyGround(Vector3 feet, out Vector3 hitPoint)
	{
		hitPoint = feet;
		if (_deadly.Count == 0)
		{
			return false;
		}
		// What Crash stands on, straight from the controller: a ray from inside his capsule hits
		// his own inner body, and one from under his feet starts below a plane he stands level
		// with, so neither ever saw the sea and he never drowned.
		Entity ground = CharacterController.GetGroundEntity(_crash);
		return ground.IsValid && _deadly.Contains(ground.Id);
	}

	internal static Vector3 Vec(JsonElement a) => new(a[0].GetSingle(), a[1].GetSingle(), a[2].GetSingle());
}
