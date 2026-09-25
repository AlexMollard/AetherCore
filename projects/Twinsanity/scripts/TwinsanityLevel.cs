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
	public float SkyScale = 7.5f;

	private const float kCrashRadius = 0.4f;
	private const float kCrashHeight = 1.8f;
	private const float kExplosionRadius = 2.5f;

	private enum Kind { Basic, Nitro, Tnt, ExtraLife, WoodenSpring, IronSpring, Iron, Checkpoint, AkuAku, MultiHit, Level, Surprise, Detonator, Reinforced }

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
	}

	private readonly List<Crate> _crates = new();
	private TwinsanityWumpa _fruit = new(_ => { }); // re-created in OnAttach with AddWumpa
	private readonly TwinsanityActors _actors = new();
	private readonly TwinsanityHud _hud = new();
	private readonly TwinsanityPause _pause = new();
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
	private int _aku;
	private int _masks = 3; // rig (logs/wildlife/monkey_hit.csv): 3 hp, one hit per hit, death at 0
	private float _deathTimer = -1.0f;
	private Entity _sky;

	public override void OnAttach()
	{
		_lives = StartLives;
		_fruit = new TwinsanityWumpa(AddWumpa);
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

		// Load the start chunk plus every chunk reachable over its chunk links (BFS, each chunk
		// once). A link's transform is relative to its own chunk, so a child's world transform is
		// local * parent (row vectors). Link targets outside the extracted set are skipped. Only
		// seamless-neighbour links (flags low byte 1) in the start chunk's own level folder stream
		// in; kind 2 is a door into another space (Doc's lab, the totem, level entrances) and kind 0
		// the boat trip - their transforms do not agree with the hub's loops.
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
		CrateFx.RegisterObjectModels(_objectModels);
		Log.Info($"[Twinsanity] {_crates.Count} crates, {_fruit.Count} wumpa, {_deadly.Count} deadly collision pieces");
		// The HUD (wumpa and lives counters, pause menu) is TwinsanityHud, fed from OnUpdate; its
		// summary has the rig evidence for when the original shows it.
	}

	public override void OnUpdate(float deltaTime)
	{
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
		UpdateFuses(deltaTime);
		DebugWarpPoll(deltaTime);
		// Keep world life and crate fx animating through the death pause, as in the original.
		_actors.Update(deltaTime, _player!, this);
		CrateFx.Update(deltaTime);
		_hud.Update(_wumpaCount, _lives, _deathTimer >= 0.0f);
		// The pause menu ticks on the unscaled clock: it freezes the game itself (Time.Scale 0)
		// while its own opening, drum and closing keep animating, as in the original.
		_pause.Update(_wumpaCount, _lives);
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
			// The sea is a mesh, so its surface is wherever the water piece sits under Crash,
			// not the chunk origin: ray down onto the piece he is standing on.
			RaycastHit sea = Physics.Raycast(feet + new Vector3(0.0f, 0.5f, 0.0f), new Vector3(0.0f, -1.0f, 0.0f), 5.0f);
			_player!.DrownSurfaceY = sea.DidHit ? sea.Position.Y : water.Y;
			Die(DeathKind.Drown); // every deadly piece in the hub is the sea
		}
	}

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

	// A level.json link entry -> the chunk's local transform (rotation, then offset), in
	// System.Numerics row-vector form.
	private static Matrix4x4 ChunkTransform(JsonElement link)
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
	private static Vector3 EulerOf(Matrix4x4 sys)
	{
		const float deg = 180.0f / MathF.PI;
		float x = MathF.Asin(Math.Clamp(-sys.M32, -1.0f, 1.0f));
		float y = MathF.Atan2(sys.M31, sys.M33);
		float z = MathF.Atan2(sys.M12, sys.M22);
		return new Vector3(x, y, z) * deg;
	}

	// Euler degrees (engine convention) -> row-vector rotation matrix: the transpose of Ry * Rx * Rz.
	private static Matrix4x4 SysRotation(Vector3 euler)
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
			_ => NameKind(model),
		};
		if (kind == null)
		{
			// Not a crate: hand it to the actor system (enemies, birds, butterflies, chickens...).
			string objectName = instance.TryGetProperty("name", out JsonElement n) ? n.GetString()! : $"object_{objectId}";
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

	// ponytail: dev-only test hook - polls project://warp.txt and teleports Crash there
	// ("x y z", any other content = idle). Inert in normal play; remove when automated
	// testing gets a proper driver API.
	private void DebugWarpPoll(float deltaTime)
	{
		_warpPoll -= deltaTime;
		if (_warpPoll > 0.0f)
		{
			return;
		}
		_warpPoll = 0.25f;
		string? text = Assets.ReadText("project://warp.txt");
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
		while (_wumpaCount >= 100)
		{
			_wumpaCount -= 100;
			_lives++;
		}
	}

	private void TouchCrates(Vector3 feet)
	{
		Vector3 velocity = _player!.Velocity;
		// Both the spin and the slide sweep break wooden crates in the original.
		bool whirled = _player.IsSpinning || _player.IsSliding;
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
			bool whirledHit = whirled && dx < 1.5f && dz < 1.5f && feet.Y < top + 0.5f && feet.Y + kCrashHeight > c.Base.Y;

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
						_checkpointFacing = _player.Facing;
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
				break;
			case Kind.AkuAku:
				_aku = Math.Min(_aku + 1, 2);
				break;
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
		if (_aku > 0)
		{
			// Aku Aku eats the hit: the original's recoil and a shove away from the hazard.
			_aku--;
			_player!.Hurt(from);
			return;
		}
		if (_masks > 1)
		{
			_masks--;
			_player!.Hurt(from);
			return;
		}
		Die(kind);
	}

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
		_aku = 0;
		_masks = 3;
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

	private static Vector3 Vec(JsonElement a) => new(a[0].GetSingle(), a[1].GetSingle(), a[2].GetSingle());
}
