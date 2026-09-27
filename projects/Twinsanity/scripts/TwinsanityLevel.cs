using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Text.Json;
using AetherCore;
using Kind = AetherGame.CrateKind;

namespace AetherGame;

/// <summary>
/// Runs a converted Twinsanity level at play time: binds the prefab instances the area scenes place
/// (TwinsanityObjects descriptors and tw_* tags, written by twinsanity.convert) and runs their rules:
/// breaking crates, collecting wumpa, lives, checkpoints and dying.
///
/// Object IDs are DefaultEnums.ObjectID from the Twinsanity editor. Crates are 1 unit cubes with
/// their origin at the bottom centre; wumpa sit about 1 unit above their origin.
/// </summary>
public sealed class TwinsanityLevel : EntityScript, TwinsanityActors.ITwinsanityHost
{
	public string LevelPath = "project://assets/levels/Earth/Hub/beach.level.json";
	public string ObjectsPath = "project://assets/levels/objects.json";
	public string PlayerName = "Crash";
	// Scene to reload on R during Play (the Playground's own name); empty = R does nothing (beach, hub).
	public string ResetScene = "";
	private bool _resetWasDown = true; // a reload starts with R still held: wait for its release
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

	// Crate kinds are CrateKind (TwinsanityObjects.cs), aliased Kind here: TwCrate serializes the same enum.

	/// <summary>The crate kind for a level instance, or null when it is not a crate the level
	/// rules handle (it then belongs to the actor system). Shared with the converter.</summary>
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
		// The body is a CHILD of the model (one selectable unit in the editor), so it follows the
		// model's transform and is never moved on its own by the fall code.
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
		// Where the bottom of its column stood at load: the fall's floor when the ground ray sees no
		// terrain (it hit a crate body still being destroyed, or started under the ground).
		public float ColumnFloor;
		// The disc instance's (layer, id), for the logs.
		public int Id = -1, Layer = -1;
		// The disc links as the target instance roots (TwCrate.Link0..9): a detonator's
		// MessageLinkedObject targets. StartOpen: the checkpoint that opens at load.
		public Entity[] LinkRoots = Array.Empty<Entity>();
		public bool StartOpen;
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
	// The level is bound on the second OnUpdate, once every scene-loaded script has attached (attach
	// order is unspecified): area scenes of prefab instances whose roots carry TwinsanityObjects descriptors.
	private int _bindFrames = 0;

	public override void OnAttach()
	{
		_lives = StartLives;
		_fruit = new TwinsanityWumpa(AddWumpa);
		LoadObjectModels();
	}

	// Frame 2 of play: every descriptor has registered. Returns false while still waiting.
	private bool BindWhenAttached()
	{
		if (_bindFrames < 0)
		{
			return true;
		}
		if (++_bindFrames < 2)
		{
			return false;
		}
		_bindFrames = -1;
		if (TwRegistry.All.Count == 0 && TwRegistry.Triggers.Count == 0 && TwRegistry.Spawns.Count == 0 && !HasConvertedContent())
		{
			Log.Error("[Twinsanity] the scene holds no converted content (no Tw* descriptors or tw_* tags) - open a converted world scene or run twinsanity.convert (docs/twinsanity-editor.md). Nothing is built.");
		}
		BindRegistry();
		FinishBuild();
		return true;
	}

	private void FinishBuild()
	{
		CrateFx.RegisterObjectModels(_objectModels);
		foreach (Crate c in _crates)
		{
			c.ColumnFloor = _crates.Where(s => OverFootprint(c, s)).Min(s => s.Base.Y);
			CrateFx.HoldRest(c.Model);
		}
		OpenStartCheckpoint();
		Log.Info($"[Twinsanity] {_crates.Count} crates, {_fruit.Count} wumpa, {_deadly.Count} deadly collision pieces");
		Action startAudio = () => TwinsanityAudio.Start(TwRegistry.All, StartArea);
		if (!IntroMovie || !_movie.Play("H01_A", startAudio))
		{
			startAudio();
		}
		TwinsanityAku.Start();
		_cutscenes.Start();
		// The HUD (wumpa and lives counters, pause menu) is TwinsanityHud, fed from OnUpdate; its
		// summary has the rig evidence for when the original shows it.
	}

	// The start chunk's stem ("beach"): the area whose DJ and ambience play, as LevelPath's level.json did.
	private string StartArea => System.IO.Path.GetFileName(LevelPath).Replace(".level.json", "", StringComparison.OrdinalIgnoreCase);

	private static bool HasConvertedContent() => Tagged("tw_collision", 1).Length > 0 || Tagged("tw_wumpa", 1).Length > 0;

	// Every entity carrying the tag (none when the tag was never registered).
	private static Entity[] Tagged(string name, int capacity = 16384)
	{
		TagId tag = Tags.Find(name);
		if (!tag.IsValid)
		{
			return Array.Empty<Entity>();
		}
		var buffer = new Entity[capacity];
		return buffer[..Tags.GetEntitiesWith(tag, buffer)];
	}

	// Binds the runtime state from the descriptors (crates, actors, spawners, cutscene agents,
	// triggers, spawn) and tags (wumpa, deadly collision, sky). Each item costs only itself if it fails.
	private void BindRegistry()
	{
		int wumpa = 0, crates = 0, actors = 0, spawners = 0;
		foreach (Entity e in Tagged("tw_wumpa"))
		{
			_fruit.Bind(e, e.Position);
			wumpa++;
		}
		TagId drownTag = Tags.Find("tw_drown");
		foreach (Entity e in Tagged("tw_deadly"))
		{
			_deadly[e.Id] = drownTag.IsValid && Tags.Has(e, drownTag);
		}
		Entity[] skies = Tagged("tw_sky");
		_sky = skies.Length > 0 ? skies[0] : default;
		if (skies.Length > 1)
		{
			Log.Warn($"[Twinsanity] {skies.Length} entities tagged tw_sky - following the camera with '{_sky.Name}' only.");
		}

		TwSpawn? spawn = null;
		foreach (TwSpawn s in TwRegistry.Spawns)
		{
			if (s.Primary)
			{
				if (spawn != null)
				{
					Log.Warn($"[Twinsanity] more than one primary TwSpawn - using '{spawn.Self.Name}'.");
					continue;
				}
				spawn = s;
			}
		}
		if (spawn != null)
		{
			_spawn = spawn.Self.Position;
			_spawnFacing = spawn.Self.EulerDegrees.Y;
			float[] floats = TwRegistry.Floats(spawn.Floats);
			_crashFloats = floats.Length > 0 ? floats : null;
			_checkpoint = _spawn;
			_checkpointFacing = _spawnFacing;
		}
		else
		{
			Log.Error("[Twinsanity] no TwSpawn with Primary set - Crash keeps his scene position and default tuning.");
		}

		foreach (TwObject o in TwRegistry.All.ToArray())
		{
			try
			{
				if (o is TwCrate c)
				{
					BindCrate(c);
					crates++;
				}
				else if (o is TwActor or TwSpawner && _actors.TryBind(o))
				{
					if (o is TwSpawner)
					{
						spawners++;
					}
					else
					{
						actors++;
					}
				}
			}
			catch (Exception ex)
			{
				Log.Warn($"[Twinsanity] descriptor bind failed on '{o.Self.Name}' ({o.GetType().Name}) - skipped ({ex.Message})");
			}
		}
		int agents = _cutscenes.BindRegistry(LevelPath[..(LevelPath.LastIndexOf('/') + 1)]);
		Log.Info($"[Twinsanity] bound registry: {actors} actors, {spawners} spawners, {crates} crates, {wumpa} wumpa, {agents} cutscene agents");
	}

	private void BindCrate(TwCrate c)
	{
		Entity e = c.Self;
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
			Kind = c.Kind,
			Body = body,
			Model = e,
			Base = e.Position,
			ObjectId = c.ObjectId,
			Id = c.Id,
			Layer = c.Layer,
			LinkRoots = Array.FindAll(c.Links(), l => l.IsValid),
			StartOpen = c.StartOpen,
		});
		// The OGI state swaps key on the crate's own model, and nitros hop.
		if (c.Model.Length > 0)
		{
			CrateFx.Spawned(e, c.ObjectId, c.Model);
		}
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

	public override void OnUpdate(float deltaTime)
	{
		if (!BindWhenAttached())
		{
			return;
		}
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
			if (!_player!.Resumed) // an R reset keeps Crash exactly where he was
			{
				_player.Respawn(_spawn, _spawnFacing);
			}
			return;
		}
		// Playground R: reload the scene (every placed object back to its saved start state) and keep Crash's
		// pose, motion, look and camera (CrashPlayer.StashForReload). Off unless the scene's Level sets
		// ResetScene, so the beach and hub never reset. Wumpa and lives restart at their start values.
		bool resetDown = Input.IsKeyDown(Key.R);
		if (resetDown && !_resetWasDown && ResetScene.Length > 0 && deltaTime > 0.0f && !_player!.IsDead)
		{
			Log.Info($"[Twinsanity] R reset: reloading '{ResetScene}', Crash kept at {_crash.Position}");
			_player.StashForReload();
			Scene.Load(ResetScene);
			return;
		}
		_resetWasDown = resetDown;

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
		foreach (Entity root in _cutscenes.TakeWakeRoots())
		{
			_actors.Wake(root);
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

	// On the rig a fresh beach load already shows the level-start checkpoint open (flat pieces, no crate
	// to hit; logs/camera/_rig_cp.png), and a death before any other checkpoint respawns him standing on
	// those pieces (game (-1.07, 0.07, -39.41); the crate is at engine (1.07, -39.41)). So it opens at load,
	// silently, and is the first checkpoint rather than the spawn. He respawns facing the crate's own
	// yaw: the rig runs off at heading 140.9 after a respawn, the crate's instance yaw is 140.87.
	private void OpenStartCheckpoint()
	{
		Crate? first = _crates.Find(c => c.StartOpen && c.Alive);
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

	// The engine composes EulerDegrees as R = Ry * Rx * Rz (column vectors); System.Numerics works
	// with row vectors, so a rotation crosses between the two as the transpose. EulerOf takes a
	// row-vector transform and returns the engine's Euler degrees of its rotation.
	// (Internal: the converter transforms instance transforms with the same pair.)
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

	private float _warpPoll;

	// ponytail: dev-only test hook - polls project://warp.txt (shared by every playing editor), or
	// instead project://warp-<AETHER_CONTROL_PORT>.txt when that file exists (this editor only), and
	// teleports Crash there ("x y z [facing]", any other content = idle). Inert in normal play; remove when
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
		if (parts.Length is 3 or 4 && float.TryParse(parts[0], out float x) && float.TryParse(parts[1], out float y) && float.TryParse(parts[2], out float z))
		{
			// An optional fourth value is the facing in degrees, which also swings the camera behind it.
			float facing = parts.Length == 4 && float.TryParse(parts[3], out float f) ? f : _player!.Facing;
			_player!.Respawn(new Vector3(x, y, z), facing);
			Log.Info($"[Twinsanity] debug warp to ({x}, {y}, {z}) facing {facing}");
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
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, s_contents.Next(1, 5)); // GetRandRange(1, 4): 1-4
				break;
			case Kind.Surprise:   // the "?" crate bursts into wumpa
			case Kind.Reinforced: // the same CreateCrateContents(0x20001, 165) as the surprise crate
				_fruit.Burst(_objectModels.GetValueOrDefault(1) ?? "", c.Base, s_contents.Next(5, 10)); // GetRandRange(5, 5): 5-9
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
			if (ground.DidHit && ground.Entity.Id == _crash.Id)
			{
				// Nor is Crash's capsule a floor: an iron that came down on him stopped on his head for good
				// (iron 49 parked at base 2.43 over him on its spring, iron_edge_steps). Look past his feet.
				ground = Physics.Raycast(new Vector3(c.Base.X, _crash.Position.Y - 0.005f, c.Base.Z), -Vector3.UnitY, 60.0f);
			}
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
			// A RISING lid also scoops him up when it sweeps through his feet this frame, wherever in the
			// crate's height they were: it climbs up to 0.76 m a frame at 30 fps (22.75 m/s), past the
			// fixed window below the lid, and the kinematic body then hit his capsule instead and flung
			// him off at the lid's speed (logs/hubb/guardred/eng_board_a.csv: thrown 2.5 m over the lid or
			// 2 m off the column; 2/24 phases boarded, rig 7/11 with a double jump, rig_board_a.csv).
			// The sweep reaches as far out as his capsule overlaps the crate's side.
			Vector3 feet = _crash.Position;
			float lid = c.Base.Y + 1.0f;
			float reach = 0.5f + (move.Y > 0.0f ? kCrashRadius : kCrashRadius * 0.75f);
			bool over = MathF.Abs(feet.X - c.Base.X) < reach && MathF.Abs(feet.Z - c.Base.Z) < reach;
			bool swept = move.Y > 0.0f && feet.Y > c.Base.Y - 0.05f && feet.Y < lid + move.Y + 0.45f;
			bool onLid = _player != null && over && (swept || (feet.Y > lid - 0.75f && feet.Y < lid + 0.45f));
			c.Base += move;
			c.Model.Position += move;
			if (onLid)
			{
				_player!.Carry(c.Base.Y + 1.0f, -c.FallSpeed, dt);
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
		CrateBlast(center, kExplosionRadius);
	}

	// An explosion's damage to the crates in range: nitro/TNT chain, checkpoints open, detonators fire, iron
	// holds, the rest break. TNT/nitro blasts and (TwinsanityActors.ITwinsanityHost) the bomb's CreateDamage
	// radius 3 (COM_GLOBAL_BOMB_DAMAGED) both land here.
	public void CrateBlast(Vector3 center, float radius)
	{
		foreach (Crate other in _crates)
		{
			if (!other.Alive || Vector3.Distance(center, other.Base + new Vector3(0.0f, 0.5f, 0.0f)) > radius)
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
	// fells log 4. Each link is the target instance root itself (TwCrate.Link*), so the crate is the one
	// whose model is that root.
	private void Detonate(Crate c)
	{
		c.Detonated = true;
		c.Fuse = -1.0f;
		CrateFx.Detonated(c.Model);
		foreach (Entity root in c.LinkRoots)
		{
			Crate? target = _crates.Find(t => t != c && t.Model.Id == root.Id);
			if (target == null)
			{
				Log.Warn($"[Twinsanity] detonator '{c.Model.Name}' link '{root.Name}' is not a crate");
				continue;
			}
			Log.Info($"[Twinsanity] detonator (layer {c.Layer}, id {c.Id}) fires {target.Kind} {target.Id} at {Vector3.Distance(c.Base, target.Base):F1} m");
			if (target.Kind is Kind.Nitro or Kind.Tnt)
			{
				Explode(target);
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

	// TwinsanityActors.ITwinsanityHost: a gem pickup fills its slot in the pause-menu gem track and
	// pops the collected gem on the HUD (AddGem).
	public void CollectGem(int slot)
	{
		_gems |= 1 << slot;
		_hud.PopGem(slot);
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

	// TwinsanityActors.ITwinsanityHost: a bomb that rolls onto a deadly surface primes (Cond125).
	public bool IsDeadly(Entity collision) => _deadly.ContainsKey(collision.Id);

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
}
