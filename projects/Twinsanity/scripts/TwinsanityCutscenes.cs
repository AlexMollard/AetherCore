using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The original's in-engine cutscenes, run from the level's own script data. tw-extract writes each chunk's
/// state machines to levels/&lt;chunk&gt;.scripts.json and its trigger volumes and director keys to the
/// .level.json; this class is a small interpreter for them (see the wiki's engine/cutscenes.md and
/// engine/scripts.md, and logs/cutscenes/inventory.md for what the Earth hub holds).
///
/// A director is an object whose script reaches COM_GENERIC_CUTSCENE_BEGIN. Its slot-0 script runs from level
/// start; a trigger volume sends it a message (87), which either starts a receiver script (the object's recv
/// table) or satisfies a GotUserMessageEquals wait. The director runs BEGIN (letterbox, control off), the scene
/// (camera, messages to actors), then END. Actors are the player and the director's linked instances: the
/// director hands them its script slots (command 171), and they draw with the director's OGI/animation tables.
///
/// Decoded (engine/scripts.md, the command objects' Read/Run functions in the PAL ELF):
///   - arguments are the command object's fields from +0xC; ints are "value &lt;&lt; 3" (bit 0: instance prop).
///   - a +control state runs a motion controller (SupportType1): NO_MOTION waits DELAY and the current clip,
///     LINEAR_INTERP moves to KEY_INDEX (252 = the focus key) at MOVE_SPEED. "Next" there, and FocusIsBusy,
///     mean the controller is still running (rig: the Cortex camera cut lands exactly when his move starts).
///   - DoAnim: arg0 bit 12 loops, 0x4000/0x8000 set the rate, 0x20000 a start fraction (PlayClip); the last word
///     packs director animation slots (clip aNNN).
///   - messages pack target (bits 16-23: 0xFB focus, 0xFF every link) and number (low 10 bits).
/// Measured on the rig (logs/cutscenes, track_*.csv): letterbox bars 15% of the frame, sliding 0.5 s.
/// </summary>
public sealed class TwinsanityCutscenes
{
	/// <summary>True while a scene holds the camera (ToggleCutsceneCamera); CrashPlayer leaves it alone.</summary>
	// Only inside a running scene: a camera toggle left on by a skipped or aborted scene can never strand the view.
	public static bool OwnsCamera => s_ownsCamera && Active;
	private static bool s_ownsCamera;

	/// <summary>True between CutsceneStart and CutsceneEnd: the letterbox is up.</summary>
	public static bool Active { get; private set; }

	private const float BarFraction = 0.15f;   // rig: 72 of 480 lines top and bottom
	private const float BarSlide = 0.5f;       // rig: 1.33 -> 1.83 s in, 14.65 -> 15.2 s out
	private const float SkipHold = 0.5f;       // Triangle held, as the mod's movie skip (30 frames at 60 Hz)
	private const int FocusTarget = 0xFB;
	private const int CheckpointMessage = 138; // a volume's message to a checkpoint / level crate
	private const int WakeMessage = 87;        // huba trigger 1's message to path crabs 9 and 10 (COM_GLOBAL_CRAB_INIT S11)
	private const int FocusKey = 252;
	private const int FocusKey2 = 246;   // KEY_INDEX 246: the key SetFocusToKey2 picked
	private const int CurrentKey = 254;  // KEY_INDEX 254 (and SetFocusToKey2's 0xFE): the SetKey/NextKey key

	// ponytail: Cmd591 (the scripted camera) is decoded only as far as its argument layout (pitch at +0x10,
	// extra distance at +0x14) - the framing maths (FUN_001102d0 and its helpers) is not. Each shot's eye and
	// look direction are the rig's, read from the EE camera matrix (0x00CFEE00) while the scene played:
	// logs/cutscenes/track_aku.csv, track_train.csv, logs/tutorial/rig_cam_s1.csv (scenes A, C), rig_cam_s2.csv (D)
	// and rig_cam_s3.csv (G). Game coordinates; x is mirrored on use. Every shot is a fixed eye held until the next
	// Cmd591 (rig_cam_s1: constant to 4 decimals). A script's shots are counted from its own CutsceneStart, so a
	// replayed scene frames the same. Scenes not in this table fall back to framing the camera subject from the
	// player's side.
	private static readonly Dictionary<string, (Vector3 Eye, Vector3 Look)[]> s_shots = new()
	{
		["COM_CUTSCENE_L01B"] = new[] { (new Vector3(-12.2527f, 1.6712f, -53.3858f), new Vector3(-0.653f, 0.0f, -0.7574f)) },
		["COM_BEACH_TRAINING_CS"] = new[]
		{
			(new Vector3(-54.0194f, 1.1007f, -60.3352f), new Vector3(-0.36f, 0.0f, 0.933f)),
			(new Vector3(-51.5224f, 2.8086f, -62.0865f), new Vector3(-0.5802f, -0.1736f, 0.7958f)),
		},
		["COM_TRAINING_CUTSCENE_A"] = new[] { (new Vector3(-6.6791f, 1.7622f, -14.9401f), new Vector3(0.0f, -0.1736f, 0.9848f)) },
		["COM_TRAINING_CUTSCENE_C"] = new[]
		{
			(new Vector3(-6.6791f, 2.5316f, -12.8680f), new Vector3(0.2588f, 0.0f, 0.9659f)),
			(new Vector3(-6.5479f, -0.5181f, 2.0527f), new Vector3(0.2418f, 0.0f, 0.9703f)),
		},
		["COM_TRAINING_CUTSCENE_D"] = new[] { (new Vector3(-14.7571f, -0.5181f, 34.9496f), new Vector3(-1.0f, 0.0f, 0.0076f)) },
		["COM_TRAINING_CUTSCENE_G"] = new[]
		{
			(new Vector3(-54.5015f, 1.6510f, 123.3484f), new Vector3(-0.1305f, 0.0f, 0.9914f)),
			(new Vector3(-51.8369f, 1.0582f, 128.9897f), new Vector3(-0.7961f, 0.0f, 0.6052f)),
			(new Vector3(-54.3942f, 1.6085f, 127.6599f), new Vector3(-0.5093f, 0.0f, 0.8606f)),
		},
	};

	// ---- data ----------------------------------------------------------------------------------

	private sealed class Rule
	{
		public int Cond, Param, To;
		public bool Not;
		public float Threshold;
		public uint[][] Cmds = Array.Empty<uint[]>();
	}

	private sealed class Motion
	{
		public string Type = "";
		public bool Translates, Rotates;
		public float Delay, Speed;          // Speed is MOVE_SPEED, or RISE_HEIGHT for PROJECTILE (the same slot)
		public float SqrTolerance, Duration, Power;
		public int Key = -1;                // KEY_INDEX: a key of the owner, or a selector (246/252 focus key, 254 current key)
		public int Selector = -1;           // SELECTOR: 251 the focus, 248 RequestFocus2's find
	}

	private sealed class State
	{
		public int Sub = -1;
		public Motion? Motion;
		public Rule[] Rules = Array.Empty<Rule>();
	}

	private sealed class ScriptDef
	{
		public string Name = "";
		public int Start;
		public State[] States = Array.Empty<State>();
	}

	private sealed class ObjectDef
	{
		public string Name = "";
		public int[] Scripts = Array.Empty<int>();
		public List<(int Message, int Script)> Recv = new();
		public int[] AnimOgi = Array.Empty<int>();   // anim slot i draws with the OGI in OGI slot i
		public Dictionary<int, string> Models = new();
	}

	private sealed class Chunk
	{
		public string Name = "";
		public Matrix4x4 Transform;
		public bool Placed;   // Transform known: given on the JSON path, solved from the first bound agent on the bake path
		public Dictionary<int, JsonElement> RawScripts = new();
		public Dictionary<int, ScriptDef?> Scripts = new();
		public Dictionary<int, ObjectDef> Objects = new();
		public Dictionary<(int Layer, int Id), Agent> Instances = new();
		public int BeginScript = -1;
		// The chunk's sound bank (tw-extract audio/sfx/<area>/<chunk>/): each object's Sounds[] slots, which
		// DoSound indexes. Loaded on the first DoSound.
		public string Bank = "";
		public Dictionary<int, int[]>? Sounds;
	}

	private sealed class Trigger
	{
		public Chunk Chunk = null!;
		public int Layer, Message;
		public Quaternion Rotation;
		public Vector3 Center, Extents;
		public int[] Targets = Array.Empty<int>();
		public bool Inside;
		// A volume starts its scene once, until a respawn at its zone's checkpoint re-arms it (Respawned).
		public bool Fired;
		public Agent? Checkpoint;   // message 138: the crate this volume makes the respawn point (huba trigger 7)
		public bool Wake;           // message 87 to path crabs: wakes them instead of running a script
	}

	private sealed class Agent
	{
		public Chunk Chunk = null!;
		public int Layer, Id, Object, Subtype;
		public int[] Params = Array.Empty<int>(); // the level instance's int params
		public uint Flags;                        // the level instance's flags (SoftFlagSet reads its bits)
		public string Name = "";
		public Vector3 Position;
		public float Yaw;                          // engine degrees
		public int[] Links = Array.Empty<int>();
		public Vector3[] Keys = Array.Empty<Vector3>();
		public bool IsPlayer, IsDirector, Done;
		public Machine? Machine;                   // the running script (slot 0 / a receiver / a handed slot)
		public Agent? Owner;                       // the director whose tables this agent draws and animates with
		public Agent? Focus;
		public int Key = 0;                        // the focus key, in the owner's keys
		public int Message = -1;
		public Entity Proxy;
		public string ProxyModel = "";
		public string Clip = "";
		public bool ClipLoops;
		public float ClipTime, ClipLength;
		public readonly Dictionary<string, int> Shots = new(); // Cmd591s each scene script has run, for the shot table
		public Vector3 Home;                       // where the level placed it (a respawn reset puts it back)
		public float HomeYaw;
		public bool Spent;                         // SetState (34): the director's scene is over for good
		public readonly HashSet<Agent> Cast = new(); // a director's actors (handed a script), which a replay resets
		public Agent? Requested;                   // RequestFocus2's find (message/hand target 0xF8)
		public Agent? CameraSubject;               // Cmd595's subject (low byte a target selector: 0xF8, 0xFB)
		public int CurKey;                         // SetKey/NextKey: the current key (KEY_INDEX 254)
		public int Hp = 1;                         // ReduceHitPoints / AgentHitPoints
		// The instance's busy flag (+4 bit 8), which condition 122 reads: RequestFocus2 claims its find
		// (ELF 0x121438) and SetObject (78, ELF 0x210DF8) sets or clears it (first argument & 3: 1 set, 2 clear).
		public bool Claimed;
		public Agent? MessageFrom;                 // who sent the last message (SetFocusToAgent's attacker)
		public Vector3 Velocity;                   // ColliderLaunchNow flight
		public bool Airborne;
	}

	private sealed class Machine
	{
		public ScriptDef Def = null!;
		public Agent Self = null!;
		public int State;
		public bool Entered;
		public float Time;
		public Machine? Sub;
		public float MotionTime;
		public bool MotionDone = true;
		public Vector3 FlightFrom, FlightTo;       // PROJECTILE: the jump's ends, launch speed and length
		public float FlightUp, FlightTime;
		public readonly HashSet<Rule> FiredInPlace = new();
		public bool Finished => Def.States.Length == 0 || (Def.States[State].Rules.Length == 0 && Def.States[State].Sub < 0 && Def.States[State].Motion == null);
		public bool Started;
		// A machine handed out this frame has not run yet: it counts as busy (the scene's FocusIsBusy wait
		// must not pass before the actor has entered its first state).
		public bool Busy => !Started || (Def.States.Length > 0 && Def.States[State].Motion != null && !MotionDone) || (Sub?.Busy ?? false);
	}

	private readonly List<Chunk> _chunks = new();
	private readonly List<Trigger> _triggers = new();
	private readonly List<Agent> _directors = new();
	private readonly Agent _player = new() { IsPlayer = true, Name = "player" };
	private readonly HashSet<string> _warned = new();
	private CrashPlayer? _crash;
	private bool _hasControl = true;
	private Entity _canvas, _top, _bottom, _fade;
	private float _bars;          // 0..1 letterbox coverage
	private float _fadeLevel, _fadeTarget, _fadeRate;
	private float _skipHeld;
	private Agent? _scene;        // the director whose cutscene holds the letterbox
	private Vector3 _camEye, _camTarget;
	private float _sceneClock;
	private int _speech;          // Audio voice id of the playing speech line
	private const float SpeechVolume = 1.0f;
	private readonly TwinsanitySkipPrompt _prompt = new();
	// BottomTextDisplay: an AgentLab line drawn in the bottom letterbox bar while a scene holds it (outside
	// one, in BottomTextShow's hint strip below). Rig
	// (logs/tutorial/rig_s1_prompt.png, 640x485): glyph cells 28 of 485 lines tall, centred at line 434.5,
	// i.e. 0.386 of the 15% bar's height, centred 0.306 of the way down it.
	private string _hint = ""; // '~' is the disc's line break
	// BottomTextShow/Hide (619/620): the gameplay hint strip the text masters use outside scenes. Rig
	// (logs/tutorialroute/rig_ch_crates.png, 640x485): the bottom bar's rect (lines 411-481) at half black,
	// two lines of the same-size glyphs (width 293 px for 427 font units = 0.38 of the bar) centred 0.2
	// and 0.48 of the way down it. ponytail: the strip fades over the command's time; the glyphs pop.
	private readonly TwinsanitySkipPrompt _prompt2 = new();
	private Entity _strip;
	private float _stripLevel, _stripTarget, _stripRate = 5.0f;
	private const float StripAlpha = 0.5f;
	private const float StripCentre1 = 0.2f, StripCentre2 = 0.48f;
	// DisplayBottomTextInstance (657) shows the agent's own line. ELF Command_DisplayBottomTextInstance_Run
	// (0x112990): n = the instance's int prop 0; AgentLab line n + 30 below 11, n + 41 from 11 on. Rig: the huba subtype-9 master
	// shows line 39 ("COLLECT AKU-AKU MASKS..."), the beach subtype-2 one line 32 ("BELLY-FLOP ON THE RED
	// BUTTON...") and the beach subtype-12 one line 53 ("JUMP ON TNT CRATES TO TRIGGER TIMER.").
	private static int InstanceTextLine(int subtype) => subtype < 11 ? subtype + 30 : subtype + 41;
	private string _hintSplitFrom = "", _hintFirst = "", _hintSecond = "", _hintOneLine = "";
	private (Vector3 Position, float Facing)? _checkpoint; // a checkpoint volume entered, for TakeCheckpoint
	private readonly List<(Vector3 At, Vector3 From)> _hits = new(); // hits on live actors, for TakeHits
	private readonly List<Vector3> _wakes = new(); // path crabs a volume woke, for TakeWakes
	private string[]? _hintLines;
	private const string HintText = "project://assets/ui/text/AgentLab/English.txt";
	private const float HintScale = 0.386f;
	private const float HintCentre = 0.306f;

	// ---- loading -------------------------------------------------------------------------------

	/// <summary>A chunk from TwinsanityLevel's BFS: its level.json root and world transform. Reads the
	/// matching .scripts.json and starts every cutscene director's slot-0 script.</summary>
	public void AddChunk(string levelPath, JsonElement level, Matrix4x4 transform)
	{
		Chunk? chunk = LoadChunkScripts(levelPath);
		if (chunk == null)
		{
			return;
		}
		chunk.Transform = transform; // the rig-shot camera table frames through it
		chunk.Placed = true;
		IngestAgents(chunk, level.GetProperty("instances"), transform);
		if (level.TryGetProperty("triggers", out JsonElement triggers))
		{
			IngestTriggers(chunk, triggers, transform);
		}
	}

	/// <summary>The object ids this chunk's scripts.json defines. TwinsanityBake offers
	/// cutscene-agent markers for the instances on these objects (and only these - the rest are
	/// pure gameplay or pure logic objects the cutscene system never touches).</summary>
	public HashSet<int> CutsceneObjectIds(string levelPath)
	{
		var ids = new HashSet<int>();
		string scriptsPath = levelPath.Replace(".level.json", ".scripts.json");
		string? text = Assets.ReadText(scriptsPath);
		if (text == null)
		{
			return ids;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		foreach (JsonProperty o in doc.RootElement.GetProperty("objects").EnumerateObject())
		{
			ids.Add(int.Parse(o.Name));
		}
		return ids;
	}

	/// <summary>Bake path: load a chunk's script data so BindAgent/BindTrigger can ingest the
	/// baked markers that name it. The markers carry world-space data; no transform is applied.</summary>
	public void BindChunkScripts(string levelPath) => LoadChunkScripts(levelPath);

	/// <summary>Bake path: one cutscene agent from its world-space marker JSON.</summary>
	public void BindAgent(JsonElement worldInstance)
	{
		if (!worldInstance.TryGetProperty("chunk", out JsonElement chunkEl))
		{
			return;
		}
		Chunk? chunk = _chunks.Find(c => c.Name == chunkEl.GetString());
		if (chunk != null && IngestAgent(chunk, worldInstance, Matrix4x4.Identity) is Agent a && !chunk.Placed)
		{
			PlaceChunk(chunk, a);
		}
	}

	// The rig shots are in the chunk's own (disc) space, but bake markers are world space. The hub's chunk links
	// are pure offsets (level.json links[].offset), so the chunk's transform is the agent's world position less
	// its level.json one.
	private static void PlaceChunk(Chunk chunk, Agent a)
	{
		chunk.Placed = true;
		if (Assets.ReadText(chunk.Name) is not string text)
		{
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		foreach (JsonElement i in doc.RootElement.GetProperty("instances").EnumerateArray())
		{
			if (i.GetProperty("layer").GetInt32() == a.Layer && i.GetProperty("id").GetInt32() == a.Id)
			{
				chunk.Transform = Matrix4x4.CreateTranslation(a.Position - Vec(i.GetProperty("position")));
				return;
			}
		}
	}

	/// <summary>Bake path: one trigger volume. Center/extents come from the marker entity's
	/// transform (what the editor gizmo edits); the rest of the record is the marker JSON.</summary>
	public void BindTrigger(JsonElement worldTrigger, Vector3 center, Vector3 extents)
	{
		if (!worldTrigger.TryGetProperty("chunk", out JsonElement chunkEl))
		{
			return;
		}
		Chunk? chunk = _chunks.Find(c => c.Name == chunkEl.GetString());
		if (chunk == null)
		{
			return;
		}
		Trigger? tr = IngestTrigger(chunk, worldTrigger, Matrix4x4.Identity);
		if (tr != null)
		{
			tr.Center = center;
			tr.Extents = extents;
		}
	}

	// The chunk's .scripts.json - its state machines, object defs and the BEGIN script id.
	// Null (with a warning) when the chunk has no extracted scripts.
	private Chunk? LoadChunkScripts(string levelPath)
	{
		string scriptsPath = levelPath.Replace(".level.json", ".scripts.json");
		string? text = Assets.ReadText(scriptsPath);
		if (text == null)
		{
			Log.Warn($"[Cutscenes] {scriptsPath} missing - re-run tw-extract for the chunk's scripts.");
			return null;
		}
		var chunk = new Chunk { Name = levelPath, Transform = Matrix4x4.Identity };
		using (JsonDocument doc = JsonDocument.Parse(text))
		{
			foreach (JsonProperty s in doc.RootElement.GetProperty("scripts").EnumerateObject())
			{
				chunk.RawScripts[int.Parse(s.Name)] = s.Value.Clone();
				if (s.Value.GetProperty("name").GetString() == "COM_GENERIC_CUTSCENE_BEGIN")
				{
					chunk.BeginScript = int.Parse(s.Name);
				}
			}
			foreach (JsonProperty o in doc.RootElement.GetProperty("objects").EnumerateObject())
			{
				chunk.Objects[int.Parse(o.Name)] = ReadObject(o.Value);
			}
		}
		_chunks.Add(chunk);
		return chunk;
	}

	private void IngestAgents(Chunk chunk, JsonElement instances, Matrix4x4 transform)
	{
		foreach (JsonElement i in instances.EnumerateArray())
		{
			IngestAgent(chunk, i, transform);
		}
	}

	private void IngestTriggers(Chunk chunk, JsonElement triggers, Matrix4x4 transform)
	{
		foreach (JsonElement t in triggers.EnumerateArray())
		{
			IngestTrigger(chunk, t, transform);
		}
	}

	// One level.json instance -> an Agent when its object carries script defs; directors are
	// flagged and collected. Returns the agent, or null when the object has no scripts.
	private Agent? IngestAgent(Chunk chunk, JsonElement i, Matrix4x4 transform)
	{
		int obj = i.GetProperty("object").GetInt32();
		if (!chunk.Objects.TryGetValue(obj, out ObjectDef? def))
		{
			return null;
		}
		var a = new Agent
		{
			Chunk = chunk,
			Layer = i.GetProperty("layer").GetInt32(),
			Id = i.GetProperty("id").GetInt32(),
			Object = obj,
			Name = i.GetProperty("name").GetString() ?? "",
			Position = Vector3.Transform(Vec(i.GetProperty("position")), transform),
			Yaw = i.GetProperty("euler")[1].GetSingle(),
			Subtype = i.TryGetProperty("subtype", out JsonElement st) ? st.GetInt32() : 0,
			Params = i.TryGetProperty("params", out JsonElement ps) ? Array.ConvertAll(ToArray(ps), e => e.GetInt32()) : Array.Empty<int>(),
			Flags = i.TryGetProperty("flags", out JsonElement fl) ? fl.GetUInt32() : 0u,
		};
		if (i.TryGetProperty("links", out JsonElement links))
		{
			a.Links = Array.ConvertAll(ToArray(links), e => e.GetInt32());
		}
		if (i.TryGetProperty("points", out JsonElement points))
		{
			a.Keys = Array.ConvertAll(ToArray(points), p => Vector3.Transform(Vec(p), transform));
		}
		a.Home = a.Position;
		a.HomeYaw = a.Yaw;
		chunk.Instances[(a.Layer, a.Id)] = a;
		if (IsDirector(chunk, def))
		{
			a.IsDirector = true;
			a.Owner = a;
			_directors.Add(a);
		}
		return a;
	}

	// One level.json trigger volume; only triggers aimed at a director matter here (crates and
	// spawners are other code's). Returns the trigger, or null when it was not kept. Center and
	// extents may be absent (the bake stores them on the marker entity's transform instead) -
	// BindTrigger overwrites both right after.
	private Trigger? IngestTrigger(Chunk chunk, JsonElement t, Matrix4x4 transform)
	{
		int header = t.GetProperty("header").GetInt32();
		var tr = new Trigger
		{
			Chunk = chunk,
			Layer = t.GetProperty("layer").GetInt32(),
			Message = (header & 0x800) != 0 ? t.GetProperty("args")[0].GetInt32() : -1,
			Center = t.TryGetProperty("center", out JsonElement center) ? Vector3.Transform(Vec(center), transform) : default,
			Extents = t.TryGetProperty("extents", out JsonElement extents) ? Vec(extents) : Vector3.One,
			Targets = Array.ConvertAll(ToArray(t.GetProperty("targets")), e => e.GetInt32()),
		};
		JsonElement q = t.GetProperty("rotation");
		tr.Rotation = Quaternion.Normalize(new Quaternion(q[0].GetSingle(), q[1].GetSingle(), q[2].GetSingle(), q[3].GetSingle()));
		// Triggers aimed at a director start scenes. Message 138 aimed at a scripted crate (the hub sends it only to
		// checkpoint and level crates) makes that crate the respawn point: huba trigger 7 and its level crate 50,
		// where the rig respawns Crash after a death (game (-3.12, 0.02, -24.07) = the crate). Other crates and
		// spawners are other code's.
		if (tr.Message >= 0 && Array.Exists(tr.Targets, id => chunk.Instances.TryGetValue((tr.Layer, id), out Agent? d) && d.IsDirector))
		{
			_triggers.Add(tr);
			return tr;
		}
		if (tr.Message == CheckpointMessage && tr.Targets.Length > 0 && chunk.Instances.TryGetValue((tr.Layer, tr.Targets[0]), out Agent? crate))
		{
			tr.Checkpoint = crate;
			_triggers.Add(tr);
			return tr;
		}
		if (tr.Message == WakeMessage && tr.Targets.Length > 0 && Array.TrueForAll(tr.Targets, id => chunk.Instances.TryGetValue((tr.Layer, id), out Agent? c)
			&& c.Name.Contains("GLOBAL_CRAB", StringComparison.Ordinal)))
		{
			tr.Wake = true;
			_triggers.Add(tr);
			return tr;
		}
		return null;
	}

	private static ObjectDef ReadObject(JsonElement o)
	{
		var def = new ObjectDef
		{
			Name = o.GetProperty("name").GetString() ?? "",
			Scripts = Array.ConvertAll(ToArray(o.GetProperty("scripts")), e => e.GetInt32()),
		};
		foreach (JsonElement r in o.GetProperty("recv").EnumerateArray())
		{
			def.Recv.Add((r[0].GetInt32(), r[1].GetInt32()));
		}
		def.AnimOgi = Array.ConvertAll(ToArray(o.GetProperty("ogis")), e => e[0].GetInt32());
		if (o.TryGetProperty("models", out JsonElement models))
		{
			foreach (JsonElement m in models.EnumerateArray())
			{
				int ogi = m.GetProperty("ogi").GetInt32();
				def.Models[ogi] = m.GetProperty("model").GetString()!;
			}
		}
		return def;
	}

	// A director runs COM_GENERIC_CUTSCENE_BEGIN somewhere below its slot-0 script or a receiver.
	private bool IsDirector(Chunk chunk, ObjectDef def)
	{
		if (chunk.BeginScript < 0)
		{
			return false;
		}
		var seen = new HashSet<int>();
		var stack = new Stack<int>();
		if (def.Scripts.Length > 0 && def.Scripts[0] != 0xFFFF)
		{
			stack.Push(def.Scripts[0]);
		}
		foreach ((int _, int script) in def.Recv)
		{
			stack.Push(script);
		}
		while (stack.Count > 0)
		{
			ScriptDef? s = Script(chunk, stack.Pop());
			if (s == null || !seen.Add(s.GetHashCode()))
			{
				continue;
			}
			foreach (State st in s.States)
			{
				if (st.Sub == chunk.BeginScript)
				{
					return true;
				}
				if (st.Sub >= 0)
				{
					stack.Push(st.Sub);
				}
			}
		}
		return false;
	}

	// A script id -> its state machine; header scripts forward to their main script.
	private static ScriptDef? Script(Chunk chunk, int id)
	{
		for (int hops = 0; hops < 4; hops++)
		{
			if (chunk.Scripts.TryGetValue(id, out ScriptDef? cached))
			{
				return cached;
			}
			if (!chunk.RawScripts.TryGetValue(id, out JsonElement raw))
			{
				return null;
			}
			if (raw.TryGetProperty("main", out JsonElement main))
			{
				id = main.GetInt32();
				continue;
			}
			var def = new ScriptDef { Name = raw.GetProperty("name").GetString() ?? "", Start = raw.GetProperty("start").GetInt32() };
			var states = new List<State>();
			foreach (JsonElement s in raw.GetProperty("states").EnumerateArray())
			{
				var st = new State { Sub = s.GetProperty("slot").GetBoolean() ? -1 : s.GetProperty("sub").GetInt32() };
				if (s.TryGetProperty("motion", out JsonElement m))
				{
					JsonElement p = m.GetProperty("params");
					st.Motion = new Motion
					{
						Type = m.GetProperty("motion").GetString() ?? "",
						Translates = m.GetProperty("translates").GetBoolean(),
						Rotates = m.GetProperty("rotates").GetBoolean(),
						Delay = p.TryGetProperty("DELAY", out JsonElement d) ? d.GetSingle() : 0.0f,
						Speed = p.TryGetProperty("MOVE_SPEED", out JsonElement v) ? v.GetSingle() : 0.0f,
						Key = p.TryGetProperty("KEY_INDEX", out JsonElement k) ? k.GetInt32() : -1,
						Selector = p.TryGetProperty("SELECTOR", out JsonElement sel) ? sel.GetInt32() : -1,
						SqrTolerance = p.TryGetProperty("SQR_TOLERANCE", out JsonElement tol) ? tol.GetSingle() : 0.0f,
						Duration = p.TryGetProperty("DURATION", out JsonElement dur) ? dur.GetSingle() : 0.0f,
						Power = p.TryGetProperty("POWER", out JsonElement pw) ? pw.GetSingle() : 0.0f,
					};
				}
				var rules = new List<Rule>();
				foreach (JsonElement r in s.GetProperty("rules").EnumerateArray())
				{
					var rule = new Rule
					{
						Cond = r.GetProperty("cond").GetInt32(),
						Param = r.GetProperty("param").GetInt32(),
						Not = r.GetProperty("not").GetBoolean(),
						Threshold = r.GetProperty("threshold").GetSingle(),
						To = r.GetProperty("to").GetInt32(),
					};
					var cmds = new List<uint[]>();
					foreach (JsonElement c in r.GetProperty("cmds").EnumerateArray())
					{
						cmds.Add(Array.ConvertAll(ToArray(c), e => e.GetUInt32()));
					}
					rule.Cmds = cmds.ToArray();
					rules.Add(rule);
				}
				st.Rules = rules.ToArray();
				states.Add(st);
			}
			def.States = states.ToArray();
			if (def.Start < 0 || def.Start >= def.States.Length)
			{
				def.Start = 0;
			}
			chunk.Scripts[id] = def;
			return def;
		}
		return null;
	}

	// ---- per frame -----------------------------------------------------------------------------

	/// <summary>Level start (after every chunk is added): the directors' default scripts start.</summary>
	public void Start()
	{
		s_ownsCamera = false;
		Active = false;
		foreach (Trigger t in _triggers)
		{
			t.Inside = t.Fired = false;
		}
		foreach (Agent d in _directors)
		{
			ObjectDef def = d.Chunk.Objects[d.Object];
			if (def.Scripts.Length > 0 && Script(d.Chunk, def.Scripts[0]) is ScriptDef s)
			{
				d.Machine = NewMachine(s, d);
			}
		}
		// The text masters (COM_UTIL_TEXTMASTER_DEFAULT) run from level start too: range checks against Crash
		// that show and hide their hint strip (huba's subtype-9 master: the Aku-Aku line at the channel crates).
		foreach (Chunk c in _chunks)
		{
			foreach (Agent a in c.Instances.Values)
			{
				ObjectDef def = c.Objects[a.Object];
				if (!a.IsDirector && def.Scripts.Length > 0 && Script(c, def.Scripts[0]) is ScriptDef s && s.Name == "COM_UTIL_TEXTMASTER_DEFAULT")
				{
					a.Machine = NewMachine(s, a);
				}
			}
		}
		int agents = 0;
		foreach (Chunk c in _chunks)
		{
			agents += c.Instances.Count;
		}
		Log.Info($"[Cutscenes] {agents} agents, {_directors.Count} directors, {_triggers.Count} trigger volumes");
	}

	/// <summary>Scene hits on live world actors since the last call: where the actor stands and where the blow
	/// came from (station 3: Coco's slide into the shieldbearer).</summary>
	public List<(Vector3 At, Vector3 From)> TakeHits()
	{
		var hits = new List<(Vector3, Vector3)>(_hits);
		_hits.Clear();
		return hits;
	}

	/// <summary>Where the path crabs a volume woke since the last call stand (message 87).</summary>
	public List<Vector3> TakeWakes()
	{
		var wakes = new List<Vector3>(_wakes);
		_wakes.Clear();
		return wakes;
	}

	/// <summary>A checkpoint volume Crash entered since the last call (message 138): the crate's position and
	/// the facing (camera yaw) he respawns with there.</summary>
	public bool TakeCheckpoint(out Vector3 position, out float facing)
	{
		(position, facing) = _checkpoint.GetValueOrDefault();
		bool any = _checkpoint.HasValue;
		_checkpoint = null;
		return any;
	}

	/// <summary>Crash has respawned at feet. At a zone checkpoint (a message-138 volume's crate: huba's level
	/// crate) the zone's scenes reset: every director of that chunk and the actors it uses go back to how the
	/// level placed them and its volumes re-arm, so the volume holding the respawn point replays at once and the
	/// others replay when entered again. Rig (logs/tutorial/rig_notes.md): a death in huba respawns Crash at
	/// the level crate inside trigger 5, station 1 replays in full, and station 2 replays on re-entering
	/// trigger 6. A director that ran SetState (34) is spent and stays so.
	/// ponytail: the reset is scoped to message-138 checkpoints because that is all the rig has shown; the
	/// beach respawns at its start checkpoint crate and resets nothing, so its scenes stay one-shot.</summary>
	public bool Respawned(Vector3 feet)
	{
		Trigger? zone = _triggers.Find(t => t.Checkpoint is Agent crate
			&& MathF.Abs(crate.Position.X - feet.X) < 0.5f && MathF.Abs(crate.Position.Z - feet.Z) < 0.5f);
		if (zone == null)
		{
			return false;
		}
		if (Active)
		{
			EndScene();
		}
		var reset = new HashSet<Agent>();
		foreach (Agent d in _directors)
		{
			if (d.Chunk != zone.Chunk || d.Spent)
			{
				continue;
			}
			reset.Add(d);
			reset.UnionWith(d.Cast);
			for (int i = 0; i < d.Links.Length; i++)
			{
				if (Linked(d, i) is Agent l)
				{
					reset.Add(l);
				}
			}
		}
		foreach (Agent a in reset)
		{
			Reset(a);
		}
		foreach (Trigger t in _triggers)
		{
			if (t.Checkpoint == null && Array.Exists(t.Targets, id => t.Chunk.Instances.TryGetValue((t.Layer, id), out Agent? d) && reset.Contains(d)))
			{
				t.Fired = t.Inside = false;
			}
		}
		Log.Info($"[Cutscenes] respawn at a zone checkpoint: {reset.Count} agents reset, the zone's scenes re-arm");
		return true;
	}

	private static bool Contains(Trigger t, Vector3 feet)
	{
		Vector3 local = Vector3.Transform(feet + new Vector3(0.0f, 0.5f, 0.0f) - t.Center, Quaternion.Conjugate(t.Rotation));
		return MathF.Abs(local.X) <= t.Extents.X && MathF.Abs(local.Y) <= t.Extents.Y && MathF.Abs(local.Z) <= t.Extents.Z;
	}

	// Back to the level's placement: the pose, no script, nothing held or found, not destroyed.
	private void Reset(Agent a)
	{
		if (a.IsPlayer)
		{
			return;
		}
		a.Position = a.Home;
		a.Yaw = a.HomeYaw;
		a.Done = false;
		a.Machine = null;
		a.Owner = a.IsDirector ? a : null;
		a.Focus = a.Requested = a.CameraSubject = a.MessageFrom = null;
		a.Message = -1;
		a.Key = a.CurKey = 0;
		a.Hp = 1;
		a.Claimed = false;
		a.Airborne = false;
		a.Velocity = Vector3.Zero;
		a.Clip = "";
		if (a.Proxy.IsValid)
		{
			a.Proxy.SetActive(false);
		}
		if (a.IsDirector)
		{
			ObjectDef def = a.Chunk.Objects[a.Object];
			if (def.Scripts.Length > 0 && Script(a.Chunk, def.Scripts[0]) is ScriptDef s)
			{
				a.Machine = NewMachine(s, a);
			}
		}
	}

	public void Update(float dt, CrashPlayer crash)
	{
		_crash = crash;
		_player.Position = crash.Self.Position;
		if (_player.Chunk == null && _chunks.Count > 0)
		{
			_player.Chunk = _chunks[0];
		}
		Vector3 feet = crash.Self.Position;
		foreach (Trigger t in _triggers)
		{
			bool inside = Contains(t, feet);
			if (inside && !t.Inside && t.Checkpoint is Agent crate)
			{
				// Every entry: walking back in after a later checkpoint makes this one current again.
				_checkpoint = (crate.Position, crate.Yaw + 180.0f);
			}
			else if (inside && !t.Inside && !t.Fired)
			{
				t.Fired = true;
				Log.Info($"[Cutscenes] volume (layer {t.Layer}, message {t.Message}) entered at {feet}");
				foreach (int id in t.Targets)
				{
					if (!t.Chunk.Instances.TryGetValue((t.Layer, id), out Agent? target))
					{
						continue;
					}
					if (t.Wake)
					{
						_wakes.Add(target.Position);
					}
					else
					{
						Deliver(target, t.Message);
					}
				}
			}
			t.Inside = inside;
		}

		UpdateSkip(dt);
		_sceneClock += Active ? dt : 0.0f;
		foreach (Agent d in _directors)
		{
			StepAgent(d, dt);
		}
		foreach (Chunk c in _chunks)
		{
			foreach (Agent a in c.Instances.Values)
			{
				if (!a.IsDirector && a.Machine != null)
				{
					StepAgent(a, dt);
				}
			}
		}
		if (_player.Machine != null)
		{
			StepAgent(_player, dt);
		}
		UpdateOverlay(dt);
		Entity camera = Camera.Main;
		if (OwnsCamera && camera.IsValid)
		{
			Camera.SetTarget(camera, _camTarget);
			Camera.SetPosition(camera, _camEye);
		}
	}

	private void StepAgent(Agent a, float dt)
	{
		if (a.Airborne)
		{
			Fly(a, dt);
		}
		if (a.Machine != null)
		{
			Step(a.Machine, dt);
		}
		AdvanceClip(a, dt);
	}

	// ColliderLaunchNow's flight: ballistic until it comes down on the ground (TouchingTerrain).
	private const float LaunchGravity = 35.0f;

	private void Fly(Agent a, float dt)
	{
		a.Velocity.Y -= LaunchGravity * dt;
		Vector3 next = a.Position + a.Velocity * dt;
		float ground = GroundY(next, float.NegativeInfinity);
		if (a.Velocity.Y < 0.0f && next.Y <= ground)
		{
			next.Y = ground;
			a.Airborne = false;
			a.Velocity = Vector3.Zero;
		}
		a.Position = next;
		PlaceProxy(a);
	}

	// The level collision's height under p (the scene's own collision meshes only, not crates or
	// Crash), or fallback when there is none within reach.
	private static float GroundY(Vector3 p, float fallback)
	{
		RaycastHit hit = Physics.Raycast(p + new Vector3(0.0f, 2.0f, 0.0f), -Vector3.UnitY, 8.0f);
		return hit.DidHit && hit.Entity.Name == "Collision" ? hit.Position.Y : fallback;
	}

	private Machine NewMachine(ScriptDef def, Agent self) => new() { Def = def, Self = self, State = def.Start };

	private void Step(Machine m, float dt)
	{
		m.Started = true;
		m.Time += dt;
		if (m.Sub != null)
		{
			Step(m.Sub, dt);
		}
		UpdateMotion(m, dt);
		for (int hop = 0; hop < 16 && m.Def.States.Length > 0; hop++)
		{
			State st = m.Def.States[m.State];
			if (!m.Entered)
			{
				Enter(m, st);
				continue;
			}
			Rule? fired = null;
			// Else (condition 2) is the fall-through wherever the list puts it: the training director's
			// subtype switch lists it first (COM_TRAINING_CUTSCENE_DIRECTOR_ACTIVATED s1).
			foreach (bool fallThrough in s_passes)
			{
				foreach (Rule r in st.Rules)
				{
					if ((r.Cond == 2) != fallThrough || (r.To < 0 && m.FiredInPlace.Contains(r)))
					{
						continue;
					}
					if (Condition(m, r) != r.Not)
					{
						fired = r;
						break;
					}
				}
				if (fired != null)
				{
					break;
				}
			}
			if (fired == null)
			{
				return;
			}
			foreach (uint[] c in fired.Cmds)
			{
				Command(m, c);
			}
			if (fired.To < 0 || fired.To >= m.Def.States.Length)
			{
				m.FiredInPlace.Add(fired);
				return;
			}
			if (m.Self.Machine == null && !m.Self.IsDirector)
			{
				return; // the command list reset this agent (172); the machine is gone
			}
			m.State = fired.To;
			m.Entered = false;
		}
	}

	private void Enter(Machine m, State st)
	{
		m.Entered = true;
		m.Time = 0.0f;
		m.FiredInPlace.Clear();
		m.Sub = st.Sub >= 0 && Script(m.Self.Chunk ?? _chunks[0], st.Sub) is ScriptDef sub ? NewMachine(sub, m.Self) : null;
		m.MotionTime = 0.0f;
		m.MotionDone = st.Motion == null;
		m.FlightTime = -1.0f;
	}

	private void UpdateMotion(Machine m, float dt)
	{
		if (m.MotionDone || m.Def.States.Length == 0 || m.Def.States[m.State].Motion is not Motion mo)
		{
			return;
		}
		Agent a = m.Self;
		m.MotionTime += dt;
		if (m.MotionTime < mo.Delay)
		{
			return;
		}
		Vector3? target = MotionTarget(a, mo);
		if (mo.Type == "PROJECTILE" && target is Vector3 land)
		{
			Jump(m, mo, land, dt);
			return;
		}
		if (mo.Type == "GROUND_CHASE" && mo.Speed > 0.0f && target is Vector3 chased)
		{
			Chase(m, mo, chased, dt);
			return;
		}
		if (mo.Translates && mo.Speed > 0.0f && target is Vector3 goal)
		{
			// LINEAR_INTERP (and, approximated, the other translating motions): straight to the key.
			Vector3 to = goal - a.Position;
			float step = mo.Speed * dt;
			if (to.Length() <= step)
			{
				a.Position = goal;
				m.MotionDone = true;
			}
			else
			{
				a.Position += Vector3.Normalize(to) * step;
			}
			PlaceProxy(a);
			return;
		}
		if (!mo.Translates && mo.Rotates && target is Vector3 look)
		{
			// A turn-only controller (Crash's L01A: face RequestFocus2's Coco). ponytail: snaps round
			// rather than turning at TURN_SPEED.
			Face(a, look);
			PlaceProxy(a);
		}
		// NO_MOTION: done once the delay has passed and a one-shot clip has played out.
		m.MotionDone = a.ClipLoops || a.Clip.Length == 0 || a.ClipTime >= a.ClipLength;
	}

	// GROUND_CHASE: run along the ground at MOVE_SPEED until within SQR_TOLERANCE of the target (which may
	// move: a SELECTOR 251 chase follows the focus), or DURATION runs out.
	private static void Chase(Machine m, Motion mo, Vector3 target, float dt)
	{
		Agent a = m.Self;
		Vector3 to = target - a.Position;
		to.Y = 0.0f;
		float tolerance = MathF.Max(mo.SqrTolerance, 0.01f);
		if (to.LengthSquared() <= tolerance || (mo.Duration > 0.0f && m.MotionTime - mo.Delay >= mo.Duration))
		{
			m.MotionDone = true;
			return;
		}
		float length = to.Length();
		Vector3 next = a.Position + to / length * MathF.Min(mo.Speed * dt, length);
		next.Y = GroundY(next, a.Position.Y + (target.Y - a.Position.Y) * MathF.Min(1.0f, mo.Speed * dt / length));
		Face(a, target);
		a.Position = next;
		PlaceProxy(a);
	}

	// PROJECTILE: a jump onto the target key, peaking RISE_HEIGHT above the higher end, under POWER gravity.
	private static void Jump(Machine m, Motion mo, Vector3 land, float dt)
	{
		Agent a = m.Self;
		float g = mo.Power > 0.0f ? mo.Power : 40.0f;
		if (m.FlightTime < 0.0f)
		{
			m.FlightFrom = a.Position;
			m.FlightTo = land;
			float apex = MathF.Max(a.Position.Y, land.Y) + MathF.Max(mo.Speed, 0.1f);
			m.FlightUp = MathF.Sqrt(2.0f * g * (apex - a.Position.Y));
			m.FlightTime = 0.0f;
			Face(a, land);
		}
		float total = m.FlightUp / g + MathF.Sqrt(2.0f * (MathF.Max(m.FlightFrom.Y, m.FlightTo.Y) + MathF.Max(mo.Speed, 0.1f) - m.FlightTo.Y) / g);
		m.FlightTime = MathF.Min(m.FlightTime + dt, total);
		float t = m.FlightTime;
		Vector3 p = Vector3.Lerp(m.FlightFrom, m.FlightTo, t / total);
		p.Y = m.FlightFrom.Y + m.FlightUp * t - 0.5f * g * t * t;
		a.Position = t >= total ? m.FlightTo : p;
		m.MotionDone = t >= total;
		PlaceProxy(a);
	}

	private static void Face(Agent a, Vector3 target)
	{
		Vector3 d = target - a.Position;
		if (d.X * d.X + d.Z * d.Z > 1e-6f)
		{
			a.Yaw = MathF.Atan2(d.X, d.Z) * 180.0f / MathF.PI;
		}
	}

	// Where a controller heads: its SELECTOR, else its KEY_INDEX (a key of the owner, or a selector).
	private Vector3? MotionTarget(Agent a, Motion mo)
	{
		int key = mo.Selector >= 0 ? mo.Selector : mo.Key;
		return key switch
		{
			FocusTarget => a.Focus?.Position,
			RequestedTarget => a.Requested?.Position,
			FocusKey or FocusKey2 => KeyPosition(a, a.Key == CurrentKey ? a.CurKey : a.Key),
			CurrentKey => KeyPosition(a, a.CurKey),
			_ => KeyPosition(a, key),
		};
	}

	// ---- conditions and commands -----------------------------------------------------------------

	private bool Condition(Machine m, Rule r)
	{
		Agent a = m.Self;
		State st = m.Def.States[m.State];
		switch (r.Cond)
		{
			case -1:
			case 0: // Next: the sub-script or motion is finished
				return (m.Sub == null || m.Sub.Finished) && m.MotionDone;
			case 2: // Else
				return true;
			case 5: // TimeInUnit
				return m.Time >= r.Threshold;
			case 7: // AnimationFinished
				return a.Clip.Length == 0 || (!a.ClipLoops && a.ClipTime >= a.ClipLength);
			case 12: // IsLoadZoneStateSet: nothing in this port reloads a zone
				return false;
			case 67: // SoftFlagSet: bit `param` of the instance's level flags. The bits differ instance by instance
			         // (huba's two seagull kinds, the text masters), so they are per-instance switches, not story flags.
				return r.Param is >= 0 and < 32 && (a.Flags >> r.Param & 1u) != 0;
			case 91: // the counter SetCounter (68) loads; the text masters' range checks read it against 6, 11, 13, 16,
			         // 21, 26, 31 for a 5-30 m radius. ponytail: taken as the instance's third param (huba's master:
			         // [9, 255, 5] -> 5 m) without decoding SetCounter's operands; the rig (rig_hint_tp.png) shows the
			         // strip 3.4 m from the master.
				return (a.Params.Length > 2 ? a.Params[2] : 0) > r.Threshold;
			case 512: // PlayerHitPoints: 1 while Crash lives, 0 once he is dead (the masks are TwinsanityAku's)
				return (_crash?.IsDead ?? false ? 0.0f : 1.0f) > r.Threshold;
			case 562: // PlayerIsCoOpLinked: no co-op link in this port
				return false;
			case 47: // ActorSubtypeEquals
				return a.Subtype == r.Param;
			case 51: // GotUserMessageEquals
				return a.Message == r.Param;
			case 65: // IsBusy
				return m.Busy;
			case 66: // FocusIsBusy
				return a.Focus?.Machine?.Busy ?? false;
			case 77: // RequestFocus2 found its object
				return a.Requested != null;
			case 122: // the requested object is busy (ELF 0x2523A8: 1.0 when there is none; 0 once it is gone,
			          // which also forgets it)
				if (a.Requested is { Done: true })
				{
					a.Requested = null;
					return false;
				}
				return a.Requested?.Claimed ?? true;
			case 56: // GotFocusObject (ELF 0x11E2A0: the focus is an object, not a position)
				return a.Focus != null;
			case 58: // GetAnimationTimeRemaining (seconds left of a one-shot clip)
				return (a.Clip.Length == 0 || a.ClipLoops ? 0.0f : MathF.Max(0.0f, a.ClipLength - a.ClipTime)) > r.Threshold;
			case 10: // MeToFocusSqrDist (ELF 0x240F70: 0 without a focus)
				return (a.Focus != null ? Vector3.DistanceSquared(a.Position, a.Focus.Position) : 0.0f) > r.Threshold;
			case 517: // MeToPlayerSqrDist
				return Vector3.DistanceSquared(a.Position, _player.Position) > r.Threshold;
			case 132: // on the last key (ELF 0x22ADE8: current key >= key count - 1)
				return KeyCount(a) > 0 && a.CurKey >= KeyCount(a) - 1;
			case 3: // Random
				return AetherCore.Random.Range(0.0f, 1.0f) > r.Threshold;
			case 524: // AgentHitPoints
				return a.Hp > r.Threshold;
			case 53: // TouchingTerrain: a launched agent has come down
				return !a.Airborne;
			case 37: // InCameraFrustrum. ponytail: taken as always true; nothing here hides off-screen agents
				return true;
			case 521: // AgentWasSpun / Slid / KneeDropped / JumpedOn: the player's attacks never reach cutscene agents
			case 522:
			case 523:
			case 535:
			case 532: // WillHitWall
			case 78:  // (creature DROP_TOOL, HIT_NONRADIUS: their rule and its Else go to the same state)
			case 106:
				return false;
			case 572: // CutsceneSkipped: taken by UpdateSkip, never polled
			case 642: // is a cutscene already running (BEGIN skips its own start when it is)
				return false;
			default:
				Warn($"condition {r.Cond} in {m.Def.Name}");
				return false;
		}
	}

	private void Command(Machine m, uint[] c)
	{
		Agent a = m.Self;
		uint Arg(int i) => i + 1 < c.Length ? c[i + 1] : 0u;
		switch (c[0])
		{
			case 556: // SetFocusToPlayer
				a.Focus = _player;
				break;
			case 564: // RequestFocus2: the nearest instance of object (arg 5 & 0xFFFF) within arg 11 of the
			          // requester (the training directors find Coco, object 412, at 20/40/400)
				a.Requested = Nearest(a.Position, (int)(Arg(5) & 0xFFFF), BitConverter.UInt32BitsToSingle(Arg(11)));
				if (a.Requested != null && (Arg(8) & 0x20000000) != 0)
				{
					a.Requested.Claimed = true; // the scenes C, D and G claim Coco; scene A leaves her to L01A's SetObject(1)
				}
				break;
			case 146: // focus a linked instance (ELF Run 0x2150D0)
				FocusLink(a, Arg(0));
				break;
			case 28: // SetFocusToKey
				a.Key = (int)Arg(0);
				break;
			case 103: // SetFocusToKey2: low byte is the key (0xFE: the current key); 0x600 are the builder's flags
				a.Key = (int)(Arg(0) & 0xFF);
				break;
			case 4: // SetKey (low byte)
				a.CurKey = (int)(Arg(0) & 0xFF);
				break;
			case 5: // NextKey
				a.CurKey = Math.Min(a.CurKey + 1, Math.Max(0, KeyCount(a) - 1));
				break;
			case 97: // forget RequestFocus2's find (ELF 0x224380 zeroes the requester's +0x118)
				a.Requested = null;
				break;
			case 108: // SetFocusPosition2: the focus becomes a point. ponytail: the point is not decoded; the
			          // tutorial's one use (Coco's L01A) is followed in the same list by SetFocusToPlayer.
				a.Focus = null;
				break;
			case 114: // stop / restart the character's own animation driver (ELF 0x2243A0 / 0x2243D0, through
			case 115: // its +0x10C controller). A scene actor here draws through its cutscene proxy instead.
				break;
			case 603: // BottomTextDisplay(AgentLab line, x, y, r, g, b, 0)
				_hint = HintLine((int)Arg(0));
				Log.Info($"[Cutscenes] prompt \"{_hint}\" ({a.Name}) at {_sceneClock:F2} s");
				break;
			case 608: // BottomTextClear
				Log.Info($"[Cutscenes] prompt cleared ({a.Name}) at {_sceneClock:F2} s");
				_hint = "";
				break;
			case 619: // BottomTextShow(fade seconds)
			case 620: // BottomTextHide(fade seconds)
				_stripTarget = c[0] == 619 ? 1.0f : 0.0f;
				_stripRate = 1.0f / MathF.Max(BitConverter.UInt32BitsToSingle(Arg(0)), 0.05f);
				break;
			case 657: // DisplayBottomTextInstance(x, y, r, g, b, 0)
				_hint = HintLine(InstanceTextLine(a.Params.Length > 0 ? a.Params[0] : a.Subtype));
				Log.Info($"[Cutscenes] hint \"{_hint}\" ({a.Name}, subtype {a.Subtype})");
				break;
			case 11: // DoSound(flags, Sounds[] slot | flags << 16, ...)
				PlaySound(a, (int)(Arg(1) & 0xFFFF));
				break;
			case 528: // ReduceHitPoints
				a.Hp -= (int)(Arg(0) >> 3);
				break;
			case 45: // SetFocusToAgent. ponytail: the hub's uses (the creature hit scripts) turn to the attacker;
			         // that is the message's sender here, the argument is not decoded.
				a.Focus = a.MessageFrom ?? a.Focus;
				break;
			case 72: // ColliderLaunchNow(.., .., .., back speed, .., ..., rise height (arg 14), ...): knocked away
			         // from the focus. ponytail: the gravity is a guess (35); args past the height are not decoded.
				Launch(a, BitConverter.UInt32BitsToSingle(Arg(3)), BitConverter.UInt32BitsToSingle(Arg(14)));
				break;
			case 85: // DestroyMe
				Destroy(a);
				break;
			case 171: // hand the target one of this director's script slots
				if (Target(a, Arg(0)) is Agent actor)
				{
					Hand(a, actor, (int)(Arg(1) & 0xFFFF));
				}
				break;
			case 172: // take it back
				if (Target(a, Arg(0)) is Agent back)
				{
					Release(back);
				}
				break;
			case 54: // SendUserMessage
				if (Target(a, Arg(0) >> 16 & 0xFF) is Agent to)
				{
					Deliver(to, (int)(Arg(0) & 0x3FF), a);
				}
				break;
			case 65: // MessageLinkedObject
			{
				int link = (int)(Arg(0) >> 16 & 0xFF);
				for (int i = 0; i < a.Links.Length; i++)
				{
					if ((link == 0xFF || link == i) && Linked(a, i) is Agent l)
					{
						Deliver(l, (int)(Arg(0) & 0x3FF), a);
					}
				}
				break;
			}
			case 66: // ClearUserMessage
				a.Message = -1;
				break;
			case 521: // SetPlayerInput(mask, mode): 0 hands control back
				SetControl(Arg(1) == 0);
				break;
			case 589: // CutsceneStart
				Log.Info($"[Cutscenes] start {m.Def.Name} ({a.Name})");
				_sceneClock = 0.0f;
				a.Shots.Clear();
				Active = true;
				_scene = a;
				SetControl(false);
				break;
			case 590: // CutsceneEnd
				Log.Info($"[Cutscenes] end ({a.Name}) after {_sceneClock:F2} s of scene time");
				EndScene();
				break;
			case 594: // ToggleCutsceneCamera: bit 0 on, bit 4 off
				if ((Arg(0) & 0x10) != 0)
				{
					s_ownsCamera = false;
				}
				else if ((Arg(0) & 1) != 0)
				{
					s_ownsCamera = true;
				}
				break;
			case 185: // start a speech line: ENGLISH.MB stream <int arg> (tw-extract --voice -> audio/voice/track_<n>.wav)
				StopSpeech();
				int track = (int)(Arg(0) >> 3);
				_speech = Audio.Play($"project://assets/audio/voice/track_{track}.wav", SpeechVolume);
				Log.Info($"[Cutscenes] speech {track} ({a.Name}) at {_sceneClock:F2} s");
				break;
			case 186: // stop the speech line
				StopSpeech();
				break;
			case 595: // the camera subject, framed by an unmeasured Cmd591 (the shot table replaces it)
				a.CameraSubject = Target(a, Arg(0));
				break;
			case 591: // the scripted camera shot
				Shot(m, (int)Arg(0));
				break;
			case 3: // PosWarp: onto the focus key
				if (KeyPosition(a, a.Key) is Vector3 warp)
				{
					a.Position = warp;
					if (a.IsPlayer)
					{
						_crash?.Respawn(warp, _crash.Facing);
					}
					PlaceProxy(a);
				}
				break;
			case 105: // PosWarp2. ponytail: turned to face the next key, which is where the hub's scenes aim him
				if (KeyPosition(a, a.Key + 1) is Vector3 face)
				{
					Vector3 d = face - a.Position;
					if (a.IsPlayer && _crash != null)
					{
						float facing = MathF.Atan2(-d.X, -d.Z) * 180.0f / MathF.PI;
						_crash.Respawn(a.Position, facing);
						a.Yaw = facing + 180.0f;
					}
					else
					{
						a.Yaw = MathF.Atan2(d.X, d.Z) * 180.0f / MathF.PI;
					}
					PlaceProxy(a);
				}
				break;
			case 9: // DoAnim
				PlayClip(a, Arg(0), Arg(5), ArgFloat(Arg(2)), ArgFloat(Arg(3)), ArgFloat(Arg(4)));
				break;
			case 602: // FadeoutScreen(mode, seconds). ponytail: a dip to black and back, which is what the
			          // hub's scene endings show; the mode bits (1, 8, 9) are not decoded.
				_fadeTarget = 1.0f;
				_fadeRate = 1.0f / MathF.Max(BitConverter.UInt32BitsToSingle(Arg(1)), 0.05f);
				break;
			case 34:  // SetState: the director is spent; its trigger no longer restarts it
				a.Done = a.Spent = true;
				break;
			case 78:  // SetObject: busy flag (see Claimed); the model follows the clip's skeleton (see ProxyFor)
				a.Claimed = (Arg(0) & 3) switch { 1 => true, 2 => false, _ => a.Claimed };
				break;
			case 515: // SetAgent flags
			case 659: // HUD / hint toggle
			case 1:   // AddTrail / ClearTrail: Cortex's flight streak, Coco's run streak
			case 2:
				break;
			case 10:  // DoParticle(0x7E81nnnn: bank index nn, ...). The tutorial's only one the rig shows is the hit
			          // flash gen_IMPACT1 (127) when Coco spins the skunk; the rest (Coco's L01C burst 42, which
			          // the rig does not show) stay unported.
				if ((Arg(0) & 0xFFFF) == 0x7F)
				{
					CrateFx.ImpactFlash(a.Position + new Vector3(0.0f, 0.8f, 0.0f));
				}
				break;
			// ponytail: visual and physics bookkeeping the tutorial's actors run that this interpreter has no
			// counterpart for: collider shape/wobble/detach, StoreCurrentSpace, RotWarp, the creature counters.
			// None of them gates a script.
			case 12:  // SetWobble
			case 13:  // ClearWobble
			case 27:  // StoreCurrentSpace
			case 29:  // RotWarp
			case 44:  // SetCollisions
			case 53:  // ClearCollisions
			case 68:  // SetCounter
			case 69:  // ModifyCounter
			case 77:  // RequestDetach
				break;
			default:
				Warn($"command {c[0]} in {m.Def.Name}");
				break;
		}
	}

	private void Deliver(Agent target, int message, Agent? from = null)
	{
		if (target.Done)
		{
			return;
		}
		target.MessageFrom = from;
		if (target.IsPlayer)
		{
			target.Message = message;
			return;
		}
		ObjectDef def = target.Chunk.Objects[target.Object];
		foreach ((int msg, int script) in def.Recv)
		{
			if (msg == message && Script(target.Chunk, script) is ScriptDef s)
			{
				if (!target.Proxy.IsValid && s.Name.EndsWith("_HIT", StringComparison.Ordinal) && from != null)
				{
					// A hit on an agent no scene has drawn: it is the live world's actor (station 3's shieldbearer,
					// whom Coco slides into). The level knocks that actor down; running the script here would
					// only draw a second copy of it.
					_hits.Add((target.Position, from.Position));
					return;
				}
				// Its own object's script: its own keys and animation tables, and no longer the director's to
				// release (scene A's message 110 starts Coco's COM_COCO_CREATURE_HUB_AFTERCUTSCENE, her run to
				// the end of her own path keys, where the rig shows her waiting for station 2).
				target.Owner = target.IsDirector ? target : null;
				target.Machine = NewMachine(s, target);
				return;
			}
		}
		target.Message = message;
	}

	// Command 146 (ELF 0x2150D0). The word: low byte a link index; 0x100 pick from the source's links, with
	// 0x8000 meaning its last link (0x200, a computed index, does not occur here); the source is the focus
	// (0x400), RequestFocus2's find (0x800), else the agent that handed this script (the director) or the
	// agent itself; bits 12-14 say where the pick goes: 0 the focus, 1 the requested slot (others: nothing).
	private void FocusLink(Agent a, uint word)
	{
		Agent? source = (word & 0x400) != 0 ? a.Focus : (word & 0x800) != 0 ? a.Requested : a.Owner ?? a;
		if (source == null || (word & 0x100) == 0 || (word & 0x200) != 0)
		{
			return;
		}
		int index = (word & 0x8000) != 0 ? source.Links.Length - 1 : (int)(word & 0xFF);
		Agent? picked = Linked(source, index);
		if (picked == null)
		{
			return;
		}
		switch (word >> 12 & 7)
		{
			case 0:
				a.Focus = picked;
				break;
			case 1:
				a.Requested = picked;
				break;
		}
	}

	private static int KeyCount(Agent a) => (a.Owner ?? a).Keys.Length;

	// DoSound: the owner's (or its own) object's Sounds[slot] from the chunk's bank, at the agent.
	private void PlaySound(Agent a, int slot)
	{
		Agent owner = a.Owner ?? a;
		Chunk chunk = owner.Chunk;
		LoadSounds(chunk);
		if (chunk.Sounds == null || !chunk.Sounds.TryGetValue(owner.Object, out int[]? slots) || slot >= slots.Length || slots[slot] == 0xFFFF)
		{
			return;
		}
		int id = slots[slot];
		Audio.PlayAt($"{chunk.Bank}{id}.wav", a.Position, 1.0f, 1.0f, false, Audio.Bus.Sfx, 4.0f, 50.0f, Audio.AttenuationModel.Linear);
	}

	private static void LoadSounds(Chunk chunk)
	{
		if (chunk.Sounds != null)
		{
			return;
		}
		chunk.Sounds = new Dictionary<int, int[]>();
		chunk.Bank = chunk.Name.Replace("/levels/", "/audio/sfx/").Replace(".level.json", "/");
		if (Assets.ReadText(chunk.Bank + "sounds.json") is not string text)
		{
			Log.Warn($"[Cutscenes] no sound bank at {chunk.Bank}");
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		foreach (JsonElement o in doc.RootElement.GetProperty("objects").EnumerateArray())
		{
			chunk.Sounds[o.GetProperty("id").GetInt32()] = Array.ConvertAll(ToArray(o.GetProperty("sounds")), e => e.GetInt32());
		}
	}

	// ColliderLaunchNow: away from the focus (the attacker) at `back` m/s (the script's negative forward
	// speed), rising `height` metres.
	private static void Launch(Agent a, float back, float height)
	{
		Vector3 away = a.Focus != null ? a.Position - a.Focus.Position : -new Vector3(MathF.Sin(a.Yaw * MathF.PI / 180.0f), 0.0f, MathF.Cos(a.Yaw * MathF.PI / 180.0f));
		away.Y = 0.0f;
		away = away.LengthSquared() > 1e-6f ? Vector3.Normalize(away) : Vector3.UnitZ;
		a.Velocity = away * MathF.Abs(back) + Vector3.UnitY * MathF.Sqrt(2.0f * LaunchGravity * MathF.Max(height, 0.05f));
		a.Airborne = true;
	}

	// DestroyMe: the agent leaves the world for good.
	private void Destroy(Agent a)
	{
		a.Done = true;
		a.Machine = null;
		a.Airborne = false;
		a.Clip = "";
		if (a.Proxy.IsValid)
		{
			a.Proxy.SetActive(false);
		}
	}

	// AgentLab line n, '~' (a line break on the disc) laid out as a space. ponytail: one line only; the
	// tutorial's prompts are all single lines.
	private string HintLine(int n)
	{
		_hintLines ??= (Assets.ReadText(HintText) ?? "").Replace("\r", "").Split('\n');
		if (n < 0 || n >= _hintLines.Length)
		{
			Warn($"AgentLab line {n} (is {HintText} extracted?)");
			return "";
		}
		return _hintLines[n];
	}

	private Agent? Target(Agent a, uint selector) => (selector & 0xFF) switch
	{
		FocusTarget => a.Focus,
		RequestedTarget => a.Requested,
		_ => Linked(a, (int)(selector & 0xFF)),
	};

	private static readonly bool[] s_passes = { false, true }; // rules first, then Else
	private const int RequestedTarget = 0xF8;

	private Agent? Nearest(Vector3 from, int obj, float range)
	{
		Agent? best = null;
		float bestSq = range * range;
		foreach (Chunk c in _chunks)
		{
			foreach (Agent x in c.Instances.Values)
			{
				float d = Vector3.DistanceSquared(x.Position, from);
				if (x.Object == obj && !x.Done && d <= bestSq)
				{
					best = x;
					bestSq = d;
				}
			}
		}
		return best;
	}

	private Agent? Linked(Agent a, int index) =>
		index >= 0 && index < a.Links.Length && a.Chunk.Instances.TryGetValue((a.Layer, a.Links[index]), out Agent? l) ? l : null;

	private void Hand(Agent director, Agent actor, int slot)
	{
		ObjectDef def = director.Chunk.Objects[director.Object];
		if (slot >= def.Scripts.Length || Script(director.Chunk, def.Scripts[slot]) is not ScriptDef s)
		{
			return;
		}
		actor.Owner = director;
		if (!actor.IsPlayer)
		{
			director.Cast.Add(actor);
		}
		actor.Key = 0;
		actor.CurKey = 0;
		actor.Message = -1;
		if (actor.IsPlayer)
		{
			actor.Chunk = director.Chunk;
			actor.Links = Array.Empty<int>();
			actor.Position = _crash?.Self.Position ?? actor.Position;
			actor.Yaw = (_crash?.Facing ?? 0.0f) + 180.0f;
		}
		actor.Machine = NewMachine(s, actor);
		Log.Info($"[Cutscenes] {s.Name} -> {actor.Name} {actor.Id} at {_sceneClock:F2} s");
	}

	private void Release(Agent actor)
	{
		actor.Machine = null;
		actor.Owner = actor.IsDirector ? actor : null;
		actor.Clip = "";
		if (actor.Proxy.IsValid)
		{
			actor.Proxy.SetActive(false);
		}
		if (actor.IsPlayer)
		{
			_crash?.SetControl(_hasControl);
		}
	}

	private void SetControl(bool on)
	{
		_hasControl = on;
		// The player's own model stays hidden while he acts in the scene (his proxy plays the clip).
		_crash?.SetControl(on && _player.Machine == null);
	}

	private void EndScene()
	{
		Active = false;
		s_ownsCamera = false;
		StopSpeech();
		if (_stripTarget <= 0.0f)
		{
			_hint = ""; // its letterbox is gone (and the actor that would clear it may be released below)
		}
		SetControl(true);
		// Actors this director still holds are let go with it (the hub's damaged skip paths never send
		// them their final message).
		if (_scene != null)
		{
			foreach (Chunk c in _chunks)
			{
				foreach (Agent a in c.Instances.Values)
				{
					if (a.Owner == _scene && !a.IsDirector)
					{
						Release(a);
					}
				}
			}
			if (_player.Owner == _scene)
			{
				Release(_player);
			}
		}
		_scene = null;
	}

	// ---- actors' models and clips --------------------------------------------------------------

	// DoAnim (ELF read 0x212028, run 0x211DF8, ProgressAnimation 0x294000). Header bits: 0x1000 loops; 0x4000 plays
	// at rate arg2 (0x10000 adds a random 0..arg3); 0x8000 stretches the clip to last arg2 seconds; 0x20000 starts at
	// arg4 (a fraction of the clip). 0x2000 (arg1) is a cross-fade time, which this port does not blend.
	private static float ArgFloat(uint v) => BitConverter.UInt32BitsToSingle(v & ~7u);

	private void PlayClip(Agent a, uint flags, uint packed, float arg2, float arg3, float start)
	{
		var slots = new List<int>();
		for (int b = 0; b < 4; b++)
		{
			int s = (int)(packed >> (8 * b) & 0xFF);
			if (s != 0xFF)
			{
				slots.Add(s);
			}
		}
		Agent owner = a.Owner ?? a;
		ObjectDef def = owner.Chunk.Objects[owner.Object];
		if (slots.Count == 0)
		{
			return;
		}
		int slot = slots[slots.Count == 1 ? 0 : AetherCore.Random.Range(0, slots.Count)];
		int ogi = slot < def.AnimOgi.Length ? def.AnimOgi[slot] : -1;
		if (!def.Models.TryGetValue(ogi, out string? model))
		{
			// The chunk does not carry that OGI (huba's director lacks 737, Coco's spin state): every OGI of
			// the object shares the skeleton and clip set, so the actor keeps the model it already shows.
			if (!a.Proxy.IsValid)
			{
				Warn($"no model for OGI {ogi} ({def.Name} clip a{slot:D3})");
				return;
			}
			model = a.ProxyModel;
		}
		if (!a.Proxy.IsValid || a.ProxyModel != model)
		{
			if (a.Proxy.IsValid)
			{
				a.Proxy.Destroy();
			}
			a.Proxy = World.Create();
			a.Proxy.Name = $"Cutscene {a.Name}";
			a.Proxy.MarkTransient();
			a.Proxy.AddTransform();
			a.Proxy.LoadModel(model);
			a.ProxyModel = model;
		}
		a.Proxy.SetActive(true);
		if (a.IsPlayer)
		{
			_crash?.SetControl(false);
		}
		a.Clip = $"a{slot:D3}";
		a.ClipLoops = (flags & 0x1000) != 0;
		int index = Animation.Find(a.Proxy, a.Clip);
		float length = 0.0f, rate = 1.0f;
		if (index >= 0)
		{
			Animation.SetClip(a.Proxy, index);
			length = Animation.ClipDuration(a.Proxy);
			if ((flags & 0x4000) != 0)
			{
				rate = arg2 + ((flags & 0x10000) != 0 ? AetherCore.Random.Range(0.0f, arg3) : 0.0f);
			}
			else if ((flags & 0x8000) != 0 && arg2 > 0.0f)
			{
				rate = length / arg2;
			}
			rate = rate > 0.0f ? rate : 1.0f;
			start = (flags & 0x20000) != 0 ? Math.Clamp(start, 0.0f, 1.0f) : 0.0f;
			Animation.SetTime(a.Proxy, start * length);
			Animation.SetPlaybackSpeed(a.Proxy, rate);
			SetLooping(a.Proxy, a.ClipLoops);
		}
		// Scene seconds: the game's clip length is frames / fps / rate, and play starts `start` of the way in.
		a.ClipLength = length / rate;
		a.ClipTime = start * a.ClipLength;
		PlaceProxy(a);
	}

	private static void AdvanceClip(Agent a, float dt)
	{
		if (a.Clip.Length > 0)
		{
			a.ClipTime += dt;
		}
	}

	private static void PlaceProxy(Agent a)
	{
		if (a.Proxy.IsValid)
		{
			a.Proxy.Position = a.Position;
			a.Proxy.EulerDegrees = new Vector3(0.0f, a.Yaw, 0.0f);
		}
	}

	// A key of the agent's owner (the director that handed it its script), or its own keys when it runs its
	// own object's scripts (Coco after a scene, the skunk).
	private static Vector3? KeyPosition(Agent a, int key)
	{
		Agent owner = a.Owner ?? a;
		return key >= 0 && key < owner.Keys.Length ? owner.Keys[key] : null;
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

	// ---- camera ------------------------------------------------------------------------------

	private void Shot(Machine m, int flags)
	{
		Agent director = m.Self;
		int index = director.Shots.GetValueOrDefault(m.Def.Name);
		director.Shots[m.Def.Name] = index + 1;
		Log.Info($"[Cutscenes] shot {m.Def.Name} #{index} at {_sceneClock:F2} s");
		if (s_shots.TryGetValue(m.Def.Name, out (Vector3 Eye, Vector3 Look)[]? shots) && index < shots.Length)
		{
			(Vector3 eye, Vector3 look) = shots[index];
			Matrix4x4 t = director.Chunk.Transform;
			_camEye = Vector3.Transform(new Vector3(-eye.X, eye.Y, eye.Z), t);
			_camTarget = _camEye + Vector3.TransformNormal(new Vector3(-look.X, look.Y, look.Z), t) * 5.0f;
		}
		else
		{
			// Unmeasured scene: frame the camera subject (the focus) from the player's side.
			Vector3 subject = (director.CameraSubject ?? director.Focus ?? _player).Position + new Vector3(0.0f, 1.2f, 0.0f);
			Vector3 away = _player.Position - subject;
			away.Y = 0.0f;
			away = away.LengthSquared() > 1e-4f ? Vector3.Normalize(away) : Vector3.UnitZ;
			_camTarget = subject;
			_camEye = subject + away * 5.0f + new Vector3(0.0f, 1.3f, 0.0f);
			Warn($"no rig shot for {m.Def.Name} #{index} (flags 0x{flags:X})");
		}
	}

	// ---- skip, letterbox, fade -----------------------------------------------------------------

	// Hold Triangle: the director takes its own skip path - the rule on condition 572 (CutsceneSkipped),
	// wherever the data left it (engine/cutscenes.md: retail stubbed 572 and orphaned most of these rules).
	private void UpdateSkip(float dt)
	{
		bool held = Gamepad.IsDown(GamepadButton.Y) || Input.IsKeyDown(Key.Enter);
		_skipHeld = held && _scene != null ? _skipHeld + dt : 0.0f;
		if (_skipHeld < SkipHold || _scene?.Machine is not Machine m)
		{
			return;
		}
		_skipHeld = 0.0f;
		for (int s = 0; s < m.Def.States.Length; s++)
		{
			foreach (Rule r in m.Def.States[s].Rules)
			{
				if (r.Cond == 572 && r.To >= 0)
				{
					Log.Info($"[Cutscenes] skip: {m.Def.Name} state {m.State} -> {r.To}");
					foreach (uint[] c in r.Cmds)
					{
						Command(m, c);
					}
					m.State = r.To;
					m.Entered = false;
					return;
				}
			}
		}
	}

	private void UpdateOverlay(float dt)
	{
		float target = Active ? 1.0f : 0.0f;
		_bars = Math.Clamp(_bars + MathF.Sign(target - _bars) * dt / BarSlide, 0.0f, 1.0f);
		if (_fadeTarget > 0.0f)
		{
			_fadeLevel = MathF.Min(1.0f, _fadeLevel + _fadeRate * dt);
			if (_fadeLevel >= 1.0f)
			{
				_fadeTarget = 0.0f;
			}
		}
		else
		{
			_fadeLevel = MathF.Max(0.0f, _fadeLevel - _fadeRate * dt);
		}
		_stripLevel = Math.Clamp(_stripLevel + MathF.Sign(_stripTarget - _stripLevel) * _stripRate * dt, 0.0f, 1.0f);
		if (_bars <= 0.0f && _fadeLevel <= 0.0f && _stripLevel <= 0.0f)
		{
			if (_canvas.IsValid)
			{
				_canvas.SetActive(false);
			}
			return;
		}
		if (!_canvas.IsValid)
		{
			_canvas = Ui.CreateCanvas();
			_canvas.MarkTransient();
			_strip = Bar();
			Ui.SetAnchors(_strip, new Vector2(0.0f, 1.0f - BarFraction), Vector2.One);
			_top = Bar();
			_bottom = Bar();
			_fade = Bar();
			Ui.SetAnchors(_fade, Vector2.Zero, Vector2.One);
		}
		_canvas.SetActive(true);
		float h = BarFraction * _bars;
		Ui.SetAnchors(_top, Vector2.Zero, new Vector2(1.0f, h));
		Ui.SetAnchors(_bottom, new Vector2(0.0f, 1.0f - h), Vector2.One);
		Ui.SetImageColor(_fade, new Vector4(0.0f, 0.0f, 0.0f, _fadeLevel));
		Ui.SetImageColor(_strip, new Vector4(0.0f, 0.0f, 0.0f, StripAlpha * _stripLevel));
		if (_hint != _hintSplitFrom)
		{
			_hintSplitFrom = _hint;
			int tilde = _hint.IndexOf('~');
			_hintFirst = tilde < 0 ? _hint : _hint[..tilde];
			_hintSecond = tilde < 0 ? "" : _hint[(tilde + 1)..];
			_hintOneLine = _hint.Replace('~', ' ');
		}
		// A scene's BottomTextDisplay takes the bar over from the port's own skip prompt (on the modded disc
		// both are the same bottom-text slot). Outside a scene the text sits in the hint strip.
		if (_hint.Length > 0 && _bars >= 1.0f)
		{
			_prompt.Show(_canvas, _bottom, _hintOneLine, HintScale, HintCentre);
			_prompt2.Show(_canvas, _strip, "", HintScale, StripCentre2);
		}
		else if (_hint.Length > 0 && _bars <= 0.0f && _stripLevel > 0.0f)
		{
			_prompt.Show(_canvas, _strip, _hintFirst, HintScale, StripCentre1);
			_prompt2.Show(_canvas, _strip, _hintSecond, HintScale, StripCentre2);
		}
		else
		{
			_prompt.Update(_canvas, _bottom, _bars >= 1.0f && CanSkip());
			_prompt2.Show(_canvas, _strip, "", HintScale, StripCentre2);
		}
	}

	private void StopSpeech()
	{
		if (_speech != 0)
		{
			Audio.Stop(_speech);
			_speech = 0;
		}
	}

	// The prompt shows only while the running scene's director has a live skip rule (condition 572).
	private bool CanSkip()
	{
		if (!Active || _scene?.Machine is not Machine m)
		{
			return false;
		}
		foreach (State st in m.Def.States)
		{
			foreach (Rule r in st.Rules)
			{
				if (r.Cond == 572 && r.To >= 0)
				{
					return true;
				}
			}
		}
		return false;
	}

	private Entity Bar()
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetOffsets(e, Vector2.Zero, Vector2.Zero);
		Ui.SetImageColor(e, new Vector4(0.0f, 0.0f, 0.0f, 1.0f));
		return e;
	}

	// ---- helpers ------------------------------------------------------------------------------

	private void Warn(string what)
	{
		if (_warned.Add(what))
		{
			Log.Warn($"[Cutscenes] unhandled {what}");
		}
	}

	private static Vector3 Vec(JsonElement e) => new(e[0].GetSingle(), e[1].GetSingle(), e[2].GetSingle());

	private static JsonElement[] ToArray(JsonElement e)
	{
		var list = new List<JsonElement>();
		foreach (JsonElement x in e.EnumerateArray())
		{
			list.Add(x);
		}
		return list.ToArray();
	}
}
