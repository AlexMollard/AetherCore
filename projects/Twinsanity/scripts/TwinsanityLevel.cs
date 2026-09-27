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
	private const float kSlamLookAhead = 2.0f / 60.0f; // two physics steps of the slam drop
	private const float kHeadContact = 0.1f;   // head within this of a lid's underside while rising = headbutt
	private const float kHeadbuttDrop = 0.44f; // m/s down after a multi-hit headbutt (rig_multi_headbutt.csv)

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
		public int Wumpa = 10;                 // MultiHit: wumpa still inside (SetCrate(0, 10))
		public int ObjectId;                   // the original data's object id (CrateFx keys on it)
		public bool Activated;                 // checkpoint crates open (keep their model) instead of breaking
		public bool CheckSupport;              // the crate under it broke or moved: see if it must fall
		public bool Falling;
		public float FallSpeed;
		// Baked crates: the body is a CHILD of the model (one selectable unit in the editor), so
		// it follows the model's transform and must not be moved again by the fall code.
		public bool BodyIsChild;
		// Where the bottom of its column stood at load: the fall's floor when the ground ray sees no
		// terrain (it hit a crate body still being destroyed, or started under the ground).
		public float ColumnFloor;
		// The disc instance's (layer, id), which other instances' links name, and its own links
		// (a detonator's MessageLinkedObject targets).
		public int Id = -1, Layer = -1;
		public int[] Links = Array.Empty<int>();
		public bool Detonated;                 // a detonator fires once (its DETONATE script ends in a control state)
	}

	private readonly List<Crate> _crates = new();
	private TwinsanityWumpa _fruit = new(_ => { }); // re-created in OnAttach with AddWumpa
	private readonly TwinsanityActors _actors = new();
	private readonly TwinsanityHud _hud = new();
	private readonly TwinsanityPause _pause = new();
	private readonly TwinsanityCutscenes _cutscenes = new();
	private readonly TwinsanityMovie _movie = new();
	// Deadly collision piece -> true for a drowning plane (surface 23), false for a pit (surface 4).
	private readonly Dictionary<uint, bool> _deadly = new();
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
		foreach (Crate c in _crates)
		{
			c.ColumnFloor = _crates.Where(s => OverFootprint(c, s)).Min(s => s.Base.Y);
		}
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
					_crates.Add(WithIdentity(new Crate
					{
						Kind = kind,
						Body = body,
						Model = e,
						Base = e.Position,
						ObjectId = json.TryGetProperty("objectId", out JsonElement oi) ? oi.GetInt32() : 0,
						BodyIsChild = true,
					}, json));
					break;
				}
				case TwinsanityBake.RoleWumpa:
					_fruit.Bind(e, e.Position);
					break;
				case TwinsanityBake.RoleDeadlyCollision:
					_deadly[e.Id] = json.TryGetProperty("drown", out JsonElement dr) && dr.GetBoolean();
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
		if (_cutscenes.TakeCheckpoint(out Vector3 checkpoint, out float checkpointFacing))
		{
			// LEVELCRATE recv 138 -> LEVEL_CRATE_OPEN: the volume's message opens the crate it names, and
			// Crash respawns on its opened lid.
			Crate? zoneCrate = _crates.Find(c => c.Kind is Kind.Checkpoint or Kind.Level
				&& MathF.Abs(c.Base.X - checkpoint.X) < 0.5f && MathF.Abs(c.Base.Z - checkpoint.Z) < 0.5f);
			if (zoneCrate != null)
			{
				Open(zoneCrate);
				checkpoint = zoneCrate.Base + new Vector3(0.0f, kOpenedTop, 0.0f);
			}
			_checkpoint = checkpoint;
			_checkpointFacing = checkpointFacing;
		}
		foreach ((Vector3 at, Vector3 from) in _cutscenes.TakeHits())
		{
			_actors.KnockEnemy(at, from);
		}
		foreach (Vector3 home in _cutscenes.TakeWakes())
		{
			_actors.Wake(home);
		}
		foreach ((Vector3 at, bool slam) in _cutscenes.TakeWormHits())
		{
			_actors.ScriptedWormHit(at, slam);
		}
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
				if (_cutscenes.Respawned(_checkpoint + new Vector3(0.0f, 0.1f, 0.0f)))
				{
					_actors.ResetGuards();
					_actors.ResetPiranhas();
				}
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
		else if (OnDeadlyGround(out bool drown))
		{
			if (!drown)
			{
				// A pit floor (surface 4, generic instant death: the huba channel pits) is the rig's
				// plain fall death, respawning at the checkpoint.
				Die(DeathKind.Fall);
				return;
			}
			// What Crash stands on here is the drowning plane (surface 23); the water surface
			// (surface 12) has no collider. In the hub collision every sea's surface sits 1.8
			// above its drowning plane (beach, huba, hubb, hubc, hubd: -1.5 over -3.3; pier
			// -216.84 over -218.64). His feet are on the plane: a ray down through him hits his
			// own inner body first (feet + 0.5, which floated him 0.5 above the rig's -1.5).
			// ponytail: a few inland pools differ (1.46 to 2.4 over their plane); export the
			// surface-12 heights per piece if those ever need the exact float height.
			_player!.DrownSurfaceY = feet.Y + WaterAboveDrownPlane;
			Die(DeathKind.Drown);
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
				_deadly[e.Id] = piece.TryGetProperty("drown", out JsonElement drown) && drown.GetBoolean();
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
		CrateFx.ShowOpened(first.Model, first.ObjectId);
		KeepOpenedCollision(first);
		_checkpoint = first.Base + new Vector3(0.0f, kOpenedTop, 0.0f);
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
		_crates.Add(WithIdentity(new Crate { Kind = kind.Value, Body = body, Model = crateModel, Base = position, ObjectId = objectId }, instance));
	}

	private static Crate WithIdentity(Crate c, JsonElement instance)
	{
		c.Id = instance.TryGetProperty("id", out JsonElement id) ? id.GetInt32() : -1;
		c.Layer = instance.TryGetProperty("layer", out JsonElement layer) ? layer.GetInt32() : -1;
		if (instance.TryGetProperty("links", out JsonElement links) && links.ValueKind == JsonValueKind.Array)
		{
			c.Links = links.EnumerateArray().Select(l => l.GetInt32()).ToArray();
		}
		return c;
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
		// the one his head met. The player's ceiling check only fires once the blocked body trails
		// the arc by its slack (~0.1 s pinned under the lid at a crate bounce's speed), so a head
		// reaching a lid's underside while rising is the headbutt too, on the frame the rig shows it
		// (logs/tutorialfinal/rig_multi_headbutt.csv).
		Crate? bonked = null;
		bool ceiling = _player.ConsumeCeilingHit();
		if (ceiling || (velocity.Y > 0.0f && !_player.IsGrounded))
		{
			float head = feet.Y + kCrashHeight;
			// Rising contact uses the lid's footprint (as onTop does), so a jump beside a crate's side is no bump.
			float reach = ceiling ? 0.5f + kCrashRadius : 0.5f + kCrashRadius * 0.75f;
			foreach (Crate c in _crates)
			{
				if (c.Alive && c.Base.Y > feet.Y && (ceiling ? c.Base.Y - feet.Y < kCrashHeight + 1.0f : head > c.Base.Y - kHeadContact)
				    && MathF.Abs(feet.X - c.Base.X) < reach && MathF.Abs(feet.Z - c.Base.Z) < reach
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
			// Touch = his capsule (plus 8 cm) reaching the box: the distance from his axis to the
			// footprint, rounded at the corners. The square band fired 0.2 m short of the Hub B red-gem
			// nitro column's corner and killed him beside the TNT (logs/hubrun/tnt_wedge.txt).
			float outX = MathF.Max(dx - 0.5f, 0.0f), outZ = MathF.Max(dz - 0.5f, 0.0f);
			bool touching = outX * outX + outZ * outZ < (kCrashRadius + 0.08f) * (kCrashRadius + 0.08f) && overlapsVertically;
			// The disc's crate behaviour slots (DefaultEnums.GameObjectScriptOrder; per-kind tables in
			// logs/craterules/crate_objects.txt): 3 touch, 4 headbutt, 5 landed on, 6 spin, 7 body slam,
			// 8 slide.
			// OnLand fires on touchdown: bouncing him while he is still dropping onto the lid left the
			// body a frame behind the new rise, which the player's ceiling check read as a bump and cut
			// a 2.5 m basic-crate bounce to ~0.9 m.
			bool landed = onTop && !_player.IsSlamming && _player.IsGrounded;
			// A body slam goes through a whole stack (rig, logs/craterules/rig_reinf_sheet.png: one slam
			// broke both reinforced crates and landed on the ground). The drop covers ~1 m a frame, so
			// the lid is struck while still ahead of the feet: the collider is gone before he lands on
			// it and the slam carries on to the next one.
			float slamReach = MathF.Max(0.0f, -velocity.Y) * kSlamLookAhead;
			bool slammed = _player.IsSlamming && dx < 0.5f + kCrashRadius * 0.75f && dz < 0.5f + kCrashRadius * 0.75f
			               && feet.Y > top - 0.75f && feet.Y - slamReach < top + 0.45f && velocity.Y < 0.5f;
			bool whirledHit = whirled && dx < 1.5f && dz < 1.5f && feet.Y < top + 0.5f && feet.Y + kCrashHeight > c.Base.Y;
			bool headbutt = c == bonked;
			bool struck = whirledHit || slammed || headbutt;

			switch (c.Kind)
			{
				case Kind.Nitro:
					// NITRO_CRATE_EXPLODE on every slot, touch included.
					if (landed || struck || touching)
					{
						Explode(c);
					}
					break;
				case Kind.Tnt:
					// TNT_CRATE_LANDED_ON bounces Crash 1.6 m and starts the fuse; every other hit is
					// TNT_CRATE_EXPLODE at once.
					if (landed)
					{
						BounceCrash(c, 1.6f);
						if (c.Fuse < 0.0f)
						{
							c.Fuse = 2.2f; // rig: boom 2.2 s after the landing bounce (tnt_fuse_sheet)
						}
					}
					else if (struck)
					{
						Explode(c);
					}
					break;
				case Kind.Basic:
				case Kind.Surprise:
				case Kind.ExtraLife:
				case Kind.AkuAku:
					// *_LANDED_ON bounces Crash and breaks the crate; every other hit is *_BREAK, with
					// no bounce.
					if (landed)
					{
						Break(c);
						BounceCrash(c, c.Kind switch { Kind.Surprise => 1.6f, Kind.ExtraLife => 2.0f, _ => 2.5f });
					}
					else if (struck)
					{
						Break(c);
					}
					break;
				case Kind.MultiHit:
					// SetCrate(0, 10): it holds 10 wumpa. A landing (bounce 3.2 m) or a headbutt pays 2
					// straight into the counter (CA_PickUpWumpa), and the one that empties it breaks it
					// (rig: two crates, 20 wumpa, both gone). A spin, slide or slam is
					// MULTIPLE_HIT_CRATE_BREAK at once, and whatever it still held is lost.
					if (landed || headbutt)
					{
						c.Wumpa -= 2;
						AddWumpa(2);
						if (c.Wumpa <= 0)
						{
							Break(c);
						}
						if (landed)
						{
							BounceCrash(c, 3.2f);
						}
						else if (c.Wumpa > 0)
						{
							// COM_MULTIPLE_HIT_CRATE_HEADBUTTED: while it still holds wumpa, ApplyVelocity
							// pushes him back down (the one that empties it breaks it, no push). The rig's
							// rise goes 4.0 -> -0.44 m/s on the bump frame and he falls from there, 14
							// frames to the lid (rig_multi_headbutt.csv), not at the script's -5.
							_player.PushDown(kHeadbuttDrop);
						}
					}
					else if (struck)
					{
						Break(c);
					}
					break;
				case Kind.Checkpoint:
				case Kind.Level:
					// CHECKPOINT_CRATE_OPEN / LEVEL_CRATE_OPEN on every slot, touch included: it opens
					// and sets the respawn point. Neither script bounces Crash.
					if (landed || struck || touching)
					{
						Open(c);
					}
					break;
				case Kind.Detonator:
					// DETONATOR_CRATE (802, logs/craterules): it never breaks and never bounces Crash (rig:
					// he stands on it, logs/hubroute/rig2_det_sheet.png). Landing on it (slot 5) runs
					// DETONATE at once; a spin (slot 6, COM_DETONATOR_CRATE_SPUN) plays a008 first. Its
					// headbutt, slam and slide slots are empty.
					if (!c.Detonated && landed)
					{
						Detonate(c);
					}
					else if (!c.Detonated && whirledHit && _player.IsSpinning)
					{
						c.Detonated = true;
						c.Fuse = CrateFx.DetonatorSpun(c.Model); // UpdateFuses detonates it once a008 ends
					}
					break;
				case Kind.Reinforced:
					// REINFORCED_WOODEN_CRATE_BREAK is only on the damage (explosions), body slam and
					// physics slots; a landing, spin, slide or headbutt is GENERIC_CRATE_SQUASH.
					if (slammed)
					{
						Break(c);
					}
					break;
				case Kind.Iron:
					break;
				case Kind.IronSpring:
					// IRON_SPRING_CRATE_LANDED_ON is on both the landing and the body slam slots.
					if (landed || slammed)
					{
						BounceCrash(c, 5.0f);
					}
					break;
				case Kind.WoodenSpring:
					if (landed)
					{
						BounceCrash(c, 5.0f);
					}
					else if (struck)
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

	private static readonly System.Random s_contents = new();

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
		// CreateCrateContents' second operand packs the wumpa count range, low nibble to high nibble:
		// basic 0x51 (1-5), surprise and reinforced 0xA5 (5-10). Rig (logs/craterules/crates.md): basic
		// crates paid 3 and 2, surprise crates 5, 5 and 8, two reinforced crates 11.
		switch (c.Kind)
		{
			case Kind.Basic:
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, s_contents.Next(1, 6));
				break;
			case Kind.Surprise:   // the "?" crate bursts into wumpa
			case Kind.Reinforced: // the same CreateCrateContents(0x20001, 165) as the surprise crate
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, s_contents.Next(5, 11));
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

	// The height operand of each LANDED_ON script's ApplyVelocity is Crash's rise in metres (rig,
	// logs/craterules/crates.md: basic and Aku 2.5 -> 2.46, surprise 1.6 -> 1.60, TNT 1.6 -> 1.57,
	// extra life 2 -> 1.98, iron and wooden springs 5 -> 4.95). The bounce arc falls under Crash's
	// air gravity, 50 (rig 49.7 m/s^2).
	private const float kBounceGravity = 50.0f;

	private void BounceCrash(Crate c, float rise)
	{
		_player!.Bounce(MathF.Sqrt(2.0f * kBounceGravity * rise));
		CrateFx.Bounced(c.Model, c.ObjectId);
	}

	// CHECKPOINT_CRATE_OPEN / LEVEL_CRATE_OPEN: the OGI flips to the opened look (245/246 and
	// 976/975) and the respawn point moves here, on the opened lid.
	private void Open(Crate c)
	{
		if (!c.Alive)
		{
			return;
		}
		c.Activated = true;
		c.Alive = false;
		KeepOpenedCollision(c);
		_checkpoint = c.Base + new Vector3(0.0f, kOpenedTop, 0.0f);
		_checkpointFacing = CheckpointFacing(c);
		Log.Info(c.Kind == Kind.Level ? "[Twinsanity] Level crate opened (level entry not implemented)." : "[Twinsanity] Checkpoint activated.");
		CrateFx.Activated(c.Model, c.ObjectId);
	}

	// Top of the opened checkpoint / level crate (CHECKPOINTCRATE_3 / LEVELCRATE_3: the lid at 0.303, the four
	// folded sides at 0.15, a 3 x 3 m cross).
	private const float kOpenedTop = 0.30f;

	// Project decision: an opened checkpoint or level crate keeps its collision so Crash can stand and jump
	// on it. The closed 1 m box gives way to the exact triangles of the opened model (state 3), placed as a
	// separate root: the state swaps replace the crate model's children. NOTE: the rig walks straight
	// through the opened beach start checkpoint at ground height (logs/tutorialfinal/rig_cp_walk.csv); the
	// collision is kept on the owner's standing decision, not the rig.
	private static void KeepOpenedCollision(Crate c)
	{
		c.Body.Destroy();
		if (CrateFx.StatePath(c.Model, c.ObjectId, 3) is not string opened)
		{
			return;
		}
		Entity b = World.Create();
		b.Name = c.Kind + " Crate (opened)";
		b.MarkTransient();
		b.AddTransform();
		b.Position = c.Model.Position;
		b.EulerDegrees = c.Model.EulerDegrees;
		Physics.AddMeshBody(b, opened);
		c.Body = b;
	}

	// Stacked crates. Rig (logs/wildlife/stack_fall_rig_raw.csv, the iron crates on the nitro stack
	// at (24.27, -7.9)): once the crate under one is gone it drops straight down, from rest, at
	// 0.0198 m/frame^2 at 50 fps (49.5 m/s^2), no bounce, and lands flush on the next support
	// (3.01 -> 0.0 on the ground, 4.01 -> 1.0 on the crate below) - except an iron on an iron spring
	// (kSpringLaunch). The crate above a falling one starts 7 frames later: it lets go once its
	// supporter's lid has dropped more than 0.7 m.
	private const float kStackGravity = 49.5f;
	private const float kStackLetGo = 0.7f;
	// An iron crate that lands on an iron spring crate bounces on it forever. Rig (HubGems,
	// logs/hubroute/rig_red_*, 50 Hz RAM on the red-gem columns' iron 49): every landing launches it at
	// 0.455 m/frame (22.75 m/s) whatever it fell from (its first drop is 1.02 m), up 5.225 m to base
	// 6.235 over the spring top at 1.01, under the same 49.5 m/s^2 gravity: a 0.92 s period.
	// ponytail: only iron on an iron spring is measured; other kinds landing on a spring come to rest.
	private const float kSpringLaunch = 22.75f;

	private Crate? SpringUnder(Crate c, float lid)
		=> _crates.Find(s => s.Alive && s.Kind == Kind.IronSpring && OverFootprint(c, s) && MathF.Abs(s.Base.Y + 1.0f - lid) < 0.05f);

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
			float floor = MathF.Max(support, terrain ? ground.Position.Y : c.ColumnFloor);
			// Exact under constant gravity, so a bounce's apex does not sag with the frame time.
			float y = c.Base.Y - (c.FallSpeed * dt + 0.5f * kStackGravity * dt * dt);
			c.FallSpeed += kStackGravity * dt;
			if (y <= floor)
			{
				Crate? spring = c.Kind == Kind.Iron ? SpringUnder(c, floor) : null;
				if (spring != null)
				{
					// Launched again off the spring's lid, forever. The launch takes up the rest of the
					// frame after the touchdown, so the period does not stretch by a frame per bounce.
					float down = c.FallSpeed - kStackGravity * dt; // speed at the frame's start
					float hit = (-down + MathF.Sqrt(down * down + 2.0f * kStackGravity * (c.Base.Y - floor))) / kStackGravity;
					float rest = Math.Clamp(dt - hit, 0.0f, dt);
					y = floor + kSpringLaunch * rest - 0.5f * kStackGravity * rest * rest;
					c.FallSpeed = -kSpringLaunch + kStackGravity * rest;
					CrateFx.Bounced(spring.Model, spring.ObjectId);
				}
				else
				{
					y = floor;
					c.Falling = false;
					c.FallSpeed = 0.0f;
				}
			}
			Vector3 move = new(0.0f, y - c.Base.Y, 0.0f);
			// Crash on the lid rides it up and down (rig: his feet stay at the iron's base + 1.02 through
			// every bounce); CrashPlayer.Carry holds him there and lets his own jump leave it.
			Vector3 feet = _crash.Position;
			bool onLid = _player != null
				&& MathF.Abs(feet.X - c.Base.X) < 0.5f + kCrashRadius * 0.75f && MathF.Abs(feet.Z - c.Base.Z) < 0.5f + kCrashRadius * 0.75f
				&& feet.Y > c.Base.Y + 1.0f - 0.75f && feet.Y < c.Base.Y + 1.0f + 0.45f;
			c.Base += move;
			if (!c.BodyIsChild)
			{
				c.Body.Position += move; // baked crates: the body is a child, it follows the model
			}
			c.Model.Position += move;
			if (onLid)
			{
				_player!.Carry(c.Base.Y + 1.0f, -c.FallSpeed);
			}
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
			else if (other.Kind is Kind.Checkpoint or Kind.Level)
			{
				Open(other); // their damage slot is the OPEN script too
			}
			else if (other.Kind == Kind.Detonator)
			{
				if (!other.Detonated)
				{
					Detonate(other); // its damage slot (2) is DETONATE too; it is not broken
				}
			}
			else if (other.Kind is not (Kind.Iron or Kind.IronSpring))
			{
				Break(other);
			}
		}
	}

	// COM_DETONATOR_CRATE_DETONATE: the plunger goes down (a009) and MessageLinkedObject(0xFF00A9) sends
	// message 169 to every link, which nitro and TNT crates receive as *_CRATE_EXPLODE (logs/craterules:
	// NITROCRATE / TNTCRATE recv 169). Rig (logs/hubroute/rig2_det_sheet.png): landing on the Hub B
	// detonator blows its linked nitro at once, the stack beside it goes in the chain, and the blast
	// fells log 4.
	private void Detonate(Crate c)
	{
		c.Detonated = true;
		c.Fuse = -1.0f;
		CrateFx.Detonated(c.Model);
		foreach (int link in c.Links)
		{
			Crate? target = LinkedCrate(c, link);
			if (target == null)
			{
				Log.Warn($"[Twinsanity] detonator (layer {c.Layer}, id {c.Id}) link {link} is not a crate here");
				continue;
			}
			Log.Info($"[Twinsanity] detonator (layer {c.Layer}, id {c.Id}) fires {target.Kind} {link} at {Vector3.Distance(c.Base, target.Base):F1} m");
			if (target.Kind is Kind.Nitro or Kind.Tnt)
			{
				Explode(target);
			}
		}
	}

	// A link names an instance of the same chunk by (layer, id). ponytail: the baked crate markers do not
	// carry their chunk, so the nearest crate with that (layer, id) stands in for the same-chunk one (every
	// hub detonator's link is a nitro 7-28 m off); record the chunk in the crate marker if two chunks' ids
	// ever collide that close.
	private Crate? LinkedCrate(Crate from, int id)
	{
		Crate? best = null;
		foreach (Crate c in _crates)
		{
			if (c != from && c.Id == id && c.Layer == from.Layer
				&& (best == null || Vector3.DistanceSquared(c.Base, from.Base) < Vector3.DistanceSquared(best.Base, from.Base)))
			{
				best = c;
			}
		}
		return best;
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
					if (c.Kind == Kind.Detonator)
					{
						Detonate(c); // a spin's a008 has played out
					}
					else
					{
						Explode(c);
					}
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
		// logs/gameplay/rig_blast1.csv: three masks, one nitro, dead in fire gibs, no knockback). So does the
		// swinging log (logs/hubb/rig_log_hit_sheet.png: dead with a mask up).
		if (kind is DeathKind.Explode or DeathKind.Crush)
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
			Log.Info($"[Twinsanity] Crash hurt ({kind}) from ({from.X:F2}, {from.Y:F2}, {from.Z:F2}), masks now {_aku}");
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

	private bool OnDeadlyGround(out bool drown)
	{
		drown = false;
		if (_deadly.Count == 0)
		{
			return false;
		}
		// What Crash stands on, straight from the controller: a ray from inside his capsule hits
		// his own inner body, and one from under his feet starts below a plane he stands level
		// with, so neither ever saw the sea and he never drowned.
		Entity ground = CharacterController.GetGroundEntity(_crash);
		return ground.IsValid && _deadly.TryGetValue(ground.Id, out drown);
	}

	internal static Vector3 Vec(JsonElement a) => new(a[0].GetSingle(), a[1].GetSingle(), a[2].GetSingle());
}
