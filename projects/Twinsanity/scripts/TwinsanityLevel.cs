using System;
using System.Collections.Generic;
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
public sealed class TwinsanityLevel : EntityScript
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

	private enum Kind { Basic, Nitro, Tnt, ExtraLife, WoodenSpring, IronSpring, Iron, Checkpoint, AkuAku }

	private sealed class Crate
	{
		public Kind Kind;
		public Entity Body;
		public Entity Model;
		public Vector3 Base;
		public bool Alive = true;
		public float Fuse = -1.0f;
	}

	private sealed class Wumpa
	{
		public Entity Model;
		public Vector3 Center;
		public float Yaw;
		public bool Alive = true;
	}

	private readonly List<Crate> _crates = new();
	private readonly List<Wumpa> _wumpa = new();
	private readonly HashSet<uint> _deadly = new();
	private readonly Dictionary<int, string> _objectModels = new();

	private Entity _crash;
	private CrashPlayer? _player;
	private Vector3 _spawn;
	private float _spawnFacing;
	private Vector3 _checkpoint;
	private float _checkpointFacing;
	private bool _placed;

	private int _wumpaCount;
	private int _lives;
	private int _aku;
	private int _cratesBroken;
	private int _cratesTotal;
	private float _deathTimer = -1.0f;
	private Entity _hud;
	private Entity _sky;
	private string _hudText = string.Empty;

	public override void OnAttach()
	{
		_lives = StartLives;
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

		string? text = Assets.ReadText(LevelPath);
		if (text == null)
		{
			Log.Error($"[Twinsanity] {LevelPath} not found - run tools/tw-extract (see docs/twinsanity-editor.md).");
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		JsonElement root = doc.RootElement;

		foreach (JsonElement path in root.GetProperty("scenery").EnumerateArray())
		{
			// Listed by naming convention; not every chunk has dynamic scenery.
			if (Assets.List(path.GetString()!).Length > 0)
			{
				Spawn("Scenery", path.GetString()!, Vector3.Zero, Vector3.Zero);
			}
		}
		// Twinsanity draws its skydome around the camera, behind everything. The extracted dome is
		// a 120-unit sphere at the chunk origin, so left in place it swallows any scenery further out.
		// Kept on the camera and grown to just inside the far plane (1000), it stays behind the world.
		_sky = Spawn("Sky", root.GetProperty("sky").GetString()!, Vector3.Zero, Vector3.Zero);
		_sky.Scale = new Vector3(SkyScale);
		for (int i = 0; i < _sky.ChildCount; i++)
		{
			MeshRenderer.SetCastShadows(_sky.GetChild(i), false);
		}

		foreach (JsonElement piece in root.GetProperty("collision").EnumerateArray())
		{
			Entity e = World.Create();
			e.Name = "Collision";
			e.AddTransform();
			Physics.AddMeshBody(e, piece.GetProperty("path").GetString()!);
			if (piece.GetProperty("deadly").GetBoolean())
			{
				_deadly.Add(e.Id);
			}
		}

		if (root.TryGetProperty("spawn", out JsonElement spawn))
		{
			_spawn = Vec(spawn.GetProperty("position"));
			// The instance yaw turns a +Z-facing model; CrashPlayer's facing is the camera yaw.
			_spawnFacing = Vec(spawn.GetProperty("euler")).Y + 180.0f;
		}
		_checkpoint = _spawn;
		_checkpointFacing = _spawnFacing;

		foreach (JsonElement instance in root.GetProperty("instances").EnumerateArray())
		{
			AddInstance(instance);
		}
		_cratesTotal = _crates.FindAll(c => c.Kind is not (Kind.Nitro or Kind.Iron or Kind.IronSpring)).Count;
		Log.Info($"[Twinsanity] {root.GetProperty("level").GetString()}: {_crates.Count} crates, {_wumpa.Count} wumpa, {_deadly.Count} deadly collision pieces");

		Entity canvas = Ui.CreateCanvas();
		_hud = Ui.CreateText(canvas, string.Empty);
		Ui.SetAnchors(_hud, new Vector2(0.0f, 0.0f), new Vector2(0.0f, 0.0f));
		Ui.SetPivot(_hud, new Vector2(0.0f, 0.0f));
		Ui.SetRect(_hud, 24.0f, 20.0f, 900.0f, 40.0f);
		Ui.SetFontSize(_hud, 30.0f);
		Ui.SetTextColor(_hud, new Vector4(1.0f, 0.85f, 0.3f, 1.0f));
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
			_player!.Respawn(_spawn, _spawnFacing);
			return;
		}

		foreach (Wumpa w in _wumpa)
		{
			if (w.Alive)
			{
				w.Yaw += 120.0f * deltaTime;
				w.Model.EulerDegrees = new Vector3(0.0f, w.Yaw, 0.0f);
			}
		}
		UpdateFuses(deltaTime);

		if (_deathTimer >= 0.0f)
		{
			_deathTimer -= deltaTime;
			if (_deathTimer < 0.0f)
			{
				_player!.Respawn(_checkpoint + new Vector3(0.0f, 0.1f, 0.0f), _checkpointFacing);
				_player.SetControl(true);
			}
			UpdateHud();
			return;
		}

		Vector3 feet = _crash.Position;
		CollectWumpa(feet);
		TouchCrates(feet);
		if (feet.Y < KillY || OnDeadlyGround(feet))
		{
			Die();
		}
		UpdateHud();
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

	private void AddInstance(JsonElement instance)
	{
		int objectId = instance.GetProperty("object").GetInt32();
		Vector3 position = Vec(instance.GetProperty("position"));
		Vector3 euler = Vec(instance.GetProperty("euler"));
		string? model = instance.TryGetProperty("model", out JsonElement m) ? m.GetString() : _objectModels.GetValueOrDefault(objectId);

		if (objectId == 1)
		{
			if (model != null)
			{
				_wumpa.Add(new Wumpa { Model = Spawn("Wumpa", model, position, euler), Center = position + new Vector3(0.0f, 1.0f, 0.0f), Yaw = euler.Y });
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
			266 => Kind.Checkpoint,
			297 => Kind.AkuAku,
			_ => null,
		};
		if (kind == null || model == null)
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
		_crates.Add(new Crate { Kind = kind.Value, Body = body, Model = Spawn(body.Name, model, position, euler), Base = position });
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

	private void CollectWumpa(Vector3 feet)
	{
		Vector3 center = feet + new Vector3(0.0f, kCrashHeight * 0.5f, 0.0f);
		foreach (Wumpa w in _wumpa)
		{
			if (w.Alive && Vector3.DistanceSquared(center, w.Center) < 1.3f * 1.3f)
			{
				w.Alive = false;
				w.Model.Destroy();
				AddWumpa(1);
			}
		}
	}

	private void AddWumpa(int count)
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
		bool spinning = _player.IsSpinning;
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

			// Standing on or landing on the lid.
			bool onTop = dx < 0.5f + kCrashRadius * 0.75f && dz < 0.5f + kCrashRadius * 0.75f
			             && feet.Y > top - 0.3f && feet.Y < top + 0.35f && velocity.Y < 0.5f;
			bool touching = dx < 0.5f + kCrashRadius + 0.08f && dz < 0.5f + kCrashRadius + 0.08f && overlapsVertically;
			bool spun = spinning && dx < 1.5f && dz < 1.5f && feet.Y < top + 0.5f && feet.Y + kCrashHeight > c.Base.Y;

			switch (c.Kind)
			{
				case Kind.Nitro:
					if (onTop || touching || spun)
					{
						Explode(c);
					}
					break;
				case Kind.Tnt:
					if (spun)
					{
						Explode(c);
					}
					else if (onTop)
					{
						_player.Bounce(9.0f);
						if (c.Fuse < 0.0f)
						{
							c.Fuse = 3.0f;
						}
					}
					break;
				case Kind.Iron:
					break;
				case Kind.IronSpring:
					if (onTop)
					{
						_player.Bounce(16.0f);
					}
					break;
				case Kind.WoodenSpring:
					if (spun)
					{
						Break(c);
					}
					else if (onTop)
					{
						_player.Bounce(16.0f);
					}
					break;
				default:
					if (spun)
					{
						Break(c);
					}
					else if (onTop)
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
		c.Model.Destroy();
		if (c.Kind is not (Kind.Nitro or Kind.Iron or Kind.IronSpring))
		{
			_cratesBroken++;
		}
		switch (c.Kind)
		{
			case Kind.Basic:
				AddWumpa(5);
				break;
			case Kind.ExtraLife:
				_lives++;
				break;
			case Kind.Checkpoint:
				_checkpoint = c.Base;
				_checkpointFacing = _player!.Facing;
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
		Vector3 center = c.Base + new Vector3(0.0f, 0.5f, 0.0f);
		Vector3 crashCenter = _crash.Position + new Vector3(0.0f, kCrashHeight * 0.5f, 0.0f);
		if (Vector3.Distance(center, crashCenter) < kExplosionRadius + kCrashRadius)
		{
			Hurt();
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

	private void Hurt()
	{
		if (_aku > 0)
		{
			_aku--;
			return;
		}
		Die();
	}

	private void Die()
	{
		if (_deathTimer >= 0.0f)
		{
			return;
		}
		_lives--;
		if (_lives < 0)
		{
			// shortcut: no game-over screen; the run simply starts over with full lives.
			_lives = StartLives;
			_wumpaCount = 0;
		}
		_aku = 0;
		_deathTimer = 1.2f;
		_player!.SetControl(false);
	}

	private bool OnDeadlyGround(Vector3 feet)
	{
		if (_deadly.Count == 0)
		{
			return false;
		}
		RaycastHit hit = Physics.Raycast(feet + new Vector3(0.0f, 0.5f, 0.0f), new Vector3(0.0f, -1.0f, 0.0f), 0.8f);
		return hit.DidHit && _deadly.Contains(hit.Entity.Id);
	}

	private void UpdateHud()
	{
		string text = $"WUMPA {_wumpaCount}    LIVES {Math.Max(_lives, 0)}    CRATES {_cratesBroken}/{_cratesTotal}{(_aku > 0 ? "    AKU AKU " + _aku : "")}";
		if (text != _hudText)
		{
			_hudText = text;
			Ui.SetText(_hud, text);
		}
	}

	private static Vector3 Vec(JsonElement a) => new(a[0].GetSingle(), a[1].GetSingle(), a[2].GetSingle());
}
