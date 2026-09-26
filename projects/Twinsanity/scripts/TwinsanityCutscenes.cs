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
///   - DoAnim: arg0 bit 12 loops, the last word packs director animation slots (clip aNNN).
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
	private const int FocusKey = 252;

	// ponytail: Cmd591 (the scripted camera) is decoded only as far as its argument layout (pitch at +0x10,
	// extra distance at +0x14) - the framing maths (FUN_001102d0 and its helpers) is not. Each shot's eye and
	// look direction are the rig's, read from the EE camera matrix (0x00CFEE00) while the scene played:
	// logs/cutscenes/track_aku.csv, track_train.csv. Game coordinates; x is mirrored on use. Scenes not in this
	// table fall back to framing the camera subject from the player's side.
	private static readonly Dictionary<string, (Vector3 Eye, Vector3 Look)[]> s_shots = new()
	{
		["COM_CUTSCENE_L01B"] = new[] { (new Vector3(-12.2527f, 1.6712f, -53.3858f), new Vector3(-0.653f, 0.0f, -0.7574f)) },
		["COM_BEACH_TRAINING_CS"] = new[]
		{
			(new Vector3(-54.0194f, 1.1007f, -60.3352f), new Vector3(-0.36f, 0.0f, 0.933f)),
			(new Vector3(-51.5224f, 2.8086f, -62.0865f), new Vector3(-0.5802f, -0.1736f, 0.7958f)),
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
		public bool Translates;
		public float Delay, Speed;
		public int Key = -1;
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
		public Dictionary<int, JsonElement> RawScripts = new();
		public Dictionary<int, ScriptDef?> Scripts = new();
		public Dictionary<int, ObjectDef> Objects = new();
		public Dictionary<(int Layer, int Id), Agent> Instances = new();
		public int BeginScript = -1;
	}

	private sealed class Trigger
	{
		public Chunk Chunk = null!;
		public int Layer, Message;
		public Quaternion Rotation;
		public Vector3 Center, Extents;
		public int[] Targets = Array.Empty<int>();
		public bool Inside;
		public bool Fired;   // a volume starts its scene once per level load (a respawn inside it re-fires nothing)
	}

	private sealed class Agent
	{
		public Chunk Chunk = null!;
		public int Layer, Id, Object, Subtype;
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
		public int Shots;                          // Cmd591s run by this agent's scene, for the shot table
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
	private readonly TwinsanitySkipPrompt _prompt = new();    // game seconds since CutsceneStart, for the log

	// ---- loading -------------------------------------------------------------------------------

	/// <summary>A chunk from TwinsanityLevel's BFS: its level.json root and world transform. Reads the
	/// matching .scripts.json and starts every cutscene director's slot-0 script.</summary>
	public void AddChunk(string levelPath, JsonElement level, Matrix4x4 transform)
	{
		string scriptsPath = levelPath.Replace(".level.json", ".scripts.json");
		string? text = Assets.ReadText(scriptsPath);
		if (text == null)
		{
			Log.Warn($"[Cutscenes] {scriptsPath} missing - re-run tw-extract for the chunk's scripts.");
			return;
		}
		var chunk = new Chunk { Name = levelPath, Transform = transform };
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

		foreach (JsonElement i in level.GetProperty("instances").EnumerateArray())
		{
			int obj = i.GetProperty("object").GetInt32();
			if (!chunk.Objects.TryGetValue(obj, out ObjectDef? def))
			{
				continue;
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
			};
			if (i.TryGetProperty("links", out JsonElement links))
			{
				a.Links = Array.ConvertAll(ToArray(links), e => e.GetInt32());
			}
			if (i.TryGetProperty("points", out JsonElement points))
			{
				a.Keys = Array.ConvertAll(ToArray(points), p => Vector3.Transform(Vec(p), transform));
			}
			chunk.Instances[(a.Layer, a.Id)] = a;
			if (IsDirector(chunk, def))
			{
				a.IsDirector = true;
				a.Owner = a;
				_directors.Add(a);
			}
		}
		if (level.TryGetProperty("triggers", out JsonElement triggers))
		{
			foreach (JsonElement t in triggers.EnumerateArray())
			{
				int header = t.GetProperty("header").GetInt32();
				var tr = new Trigger
				{
					Chunk = chunk,
					Layer = t.GetProperty("layer").GetInt32(),
					Message = (header & 0x800) != 0 ? t.GetProperty("args")[0].GetInt32() : -1,
					Center = Vector3.Transform(Vec(t.GetProperty("center")), transform),
					Extents = Vec(t.GetProperty("extents")),
					Targets = Array.ConvertAll(ToArray(t.GetProperty("targets")), e => e.GetInt32()),
				};
				JsonElement q = t.GetProperty("rotation");
				tr.Rotation = Quaternion.Normalize(new Quaternion(q[0].GetSingle(), q[1].GetSingle(), q[2].GetSingle(), q[3].GetSingle()));
				// Only triggers aimed at a director matter here; crates and spawners are other code's.
				if (tr.Message >= 0 && Array.Exists(tr.Targets, id => chunk.Instances.TryGetValue((tr.Layer, id), out Agent? d) && d.IsDirector))
				{
					_triggers.Add(tr);
				}
			}
		}
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
						Delay = p.TryGetProperty("DELAY", out JsonElement d) ? d.GetSingle() : 0.0f,
						Speed = p.TryGetProperty("MOVE_SPEED", out JsonElement v) ? v.GetSingle() : 0.0f,
						Key = p.TryGetProperty("KEY_INDEX", out JsonElement k) ? k.GetInt32() : -1,
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
		Log.Info($"[Cutscenes] {_directors.Count} directors, {_triggers.Count} trigger volumes");
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
			Vector3 local = Vector3.Transform(feet + new Vector3(0.0f, 0.5f, 0.0f) - t.Center, Quaternion.Conjugate(t.Rotation));
			bool inside = MathF.Abs(local.X) <= t.Extents.X && MathF.Abs(local.Y) <= t.Extents.Y && MathF.Abs(local.Z) <= t.Extents.Z;
			if (inside && !t.Inside && !t.Fired)
			{
				t.Fired = true;
				foreach (int id in t.Targets)
				{
					if (t.Chunk.Instances.TryGetValue((t.Layer, id), out Agent? target))
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
		if (a.Machine != null)
		{
			Step(a.Machine, dt);
		}
		AdvanceClip(a, dt);
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
			foreach (Rule r in st.Rules)
			{
				if (r.To < 0 && m.FiredInPlace.Contains(r))
				{
					continue;
				}
				if (Condition(m, r) != r.Not)
				{
					fired = r;
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
		if (mo.Translates && mo.Speed > 0.0f && KeyPosition(a, mo.Key == FocusKey ? a.Key : mo.Key) is Vector3 goal)
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
		// NO_MOTION: done once the delay has passed and a one-shot clip has played out.
		m.MotionDone = a.ClipLoops || a.Clip.Length == 0 || a.ClipTime >= a.ClipLength;
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
			case 67: // SoftFlagSet: story flags; a fresh game has none
				return false;
			case 47: // ActorSubtypeEquals
				return a.Subtype == r.Param;
			case 51: // GotUserMessageEquals
				return a.Message == r.Param;
			case 65: // IsBusy
				return m.Busy;
			case 66: // FocusIsBusy
				return a.Focus?.Machine?.Busy ?? false;
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
			case 146: // focus a linked instance. ponytail: only 256 (the first link) occurs on the hub.
				a.Focus = Linked(a, Math.Max(0, (int)(Arg(0) >> 8) - 1));
				break;
			case 28: // SetFocusToKey
				a.Key = (int)Arg(0);
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
					Deliver(to, (int)(Arg(0) & 0x3FF));
				}
				break;
			case 65: // MessageLinkedObject
			{
				int link = (int)(Arg(0) >> 16 & 0xFF);
				for (int i = 0; i < a.Links.Length; i++)
				{
					if ((link == 0xFF || link == i) && Linked(a, i) is Agent l)
					{
						Deliver(l, (int)(Arg(0) & 0x3FF));
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
				_speech = Audio.Play($"project://assets/audio/voice/track_{Arg(0) >> 3}.wav", SpeechVolume);
				Log.Info($"[Cutscenes] speech {Arg(0) >> 3} ({a.Name}) at {_sceneClock:F2} s");
				break;
			case 186: // stop the speech line
				StopSpeech();
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
				PlayClip(a, Arg(0), Arg(5));
				break;
			case 602: // FadeoutScreen(mode, seconds). ponytail: a dip to black and back, which is what the
			          // hub's scene endings show; the mode bits (1, 8, 9) are not decoded.
				_fadeTarget = 1.0f;
				_fadeRate = 1.0f / MathF.Max(BitConverter.UInt32BitsToSingle(Arg(1)), 0.05f);
				break;
			case 34:  // SetState: the director is spent; its trigger no longer restarts it
				a.Done = true;
				break;
			case 78:  // SetObject: the model follows the clip's skeleton (see ProxyFor)
			case 515: // SetAgent flags
			case 595: // camera subject (the shot table replaces the framing)
			case 659: // HUD / hint toggle
			case 1:   // AddTrail / ClearTrail: Cortex's flight streak
			case 2:
				break;
			default:
				Warn($"command {c[0]} in {m.Def.Name}");
				break;
		}
	}

	private void Deliver(Agent target, int message)
	{
		if (target.Done)
		{
			return;
		}
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
				target.Machine = NewMachine(s, target);
				return;
			}
		}
		target.Message = message;
	}

	private Agent? Target(Agent a, uint selector) => (selector & 0xFF) switch
	{
		FocusTarget => a.Focus,
		_ => Linked(a, (int)(selector & 0xFF)),
	};

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
		actor.Key = 0;
		actor.Message = -1;
		if (actor.IsPlayer)
		{
			actor.Chunk = director.Chunk;
			actor.Links = Array.Empty<int>();
			actor.Position = _crash?.Self.Position ?? actor.Position;
			actor.Yaw = (_crash?.Facing ?? 0.0f) + 180.0f;
		}
		actor.Machine = NewMachine(s, actor);
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

	private void PlayClip(Agent a, uint flags, uint packed)
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
			Warn($"no model for OGI {ogi} ({def.Name} clip a{slot:D3})");
			return;
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
		a.ClipTime = 0.0f;
		int index = Animation.Find(a.Proxy, a.Clip);
		if (index >= 0)
		{
			Animation.SetClip(a.Proxy, index);
			Animation.SetTime(a.Proxy, 0.0f);
			Animation.SetPlaybackSpeed(a.Proxy, 1.0f);
			SetLooping(a.Proxy, a.ClipLoops);
			a.ClipLength = Animation.ClipDuration(a.Proxy);
		}
		else
		{
			a.ClipLength = 0.0f;
		}
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

	private static Vector3? KeyPosition(Agent a, int key)
	{
		Agent? owner = a.Owner;
		return owner != null && key >= 0 && key < owner.Keys.Length ? owner.Keys[key] : null;
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
		if (s_shots.TryGetValue(m.Def.Name, out (Vector3 Eye, Vector3 Look)[]? shots) && director.Shots < shots.Length)
		{
			(Vector3 eye, Vector3 look) = shots[director.Shots];
			Matrix4x4 t = director.Chunk.Transform;
			_camEye = Vector3.Transform(new Vector3(-eye.X, eye.Y, eye.Z), t);
			_camTarget = _camEye + Vector3.TransformNormal(new Vector3(-look.X, look.Y, look.Z), t) * 5.0f;
		}
		else
		{
			// Unmeasured scene: frame the camera subject (the focus) from the player's side.
			Vector3 subject = (director.Focus ?? _player).Position + new Vector3(0.0f, 1.2f, 0.0f);
			Vector3 away = _player.Position - subject;
			away.Y = 0.0f;
			away = away.LengthSquared() > 1e-4f ? Vector3.Normalize(away) : Vector3.UnitZ;
			_camTarget = subject;
			_camEye = subject + away * 5.0f + new Vector3(0.0f, 1.3f, 0.0f);
			Warn($"no rig shot for {m.Def.Name} #{director.Shots} (flags 0x{flags:X})");
		}
		director.Shots++;
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
		if (_bars <= 0.0f && _fadeLevel <= 0.0f)
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
		_prompt.Update(_canvas, _bottom, _bars >= 1.0f && CanSkip());
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
