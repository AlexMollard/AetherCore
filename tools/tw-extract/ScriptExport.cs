using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Twinsanity;
using static Twinsanity.Script.MainScript;
using Path = System.IO.Path;

namespace TwExtract
{
	// levels/<chunk>.scripts.json: the chunk's script state machines and the object tables a runtime needs to
	// run them (the cutscene player, TwinsanityCutscenes.cs, interprets these generically).
	//   objects: id -> name (full group path), script slots, trigger receivers [message, script], anim/OGI slot
	//            tables with each entry's joint count, and the exported model per OGI id.
	//   scripts: id -> {name, main} for a header script, or {name, start, states} for a main script. A state is
	//            {bits, sub (sub-script id, or slot when "slot"), control, rules}; a rule is {cond, param, not,
	//            interval, threshold, bits, to (-1: none), cmds: [[command id, raw argument words...]]}.
	// Arguments are the command object's fields from +0xC as the game lays them out (GetScriptIntArg etc. decode
	// them), so they are exported raw rather than guessed at.
	static class ScriptExport
	{
		public static string Write(TwinsFile file, string level, string outRoot, Dictionary<uint, List<(uint Ogi, string Model)>> objectModels)
		{
			var items = Gfx.Items(file).ToList();
			var ogiJoints = items.OfType<GraphicsInfo>().GroupBy(g => g.ID).ToDictionary(g => g.Key, g => g.First().Joints.Length);
			var animJoints = items.OfType<Animation>().GroupBy(a => a.ID).ToDictionary(g => g.Key, g => g.First().JointsSettings.Count);
			var objects = new Dictionary<string, object>();
			foreach (var o in items.OfType<GameObject>().GroupBy(o => o.ID).Select(g => g.First()))
			{
				var row = new Dictionary<string, object>
				{
					["name"] = o.Name,
					["scripts"] = o.Scripts.Select(s => (int)s).ToList(),
					["recv"] = o.UI32.Select(v => new[] { (int)(v & 0x3FF), (int)((v >> 10) & 0x3FFF) }).ToList(),
					["anims"] = o.Anims.Select(a => new[] { (int)a, animJoints.TryGetValue(a, out int n) ? n : 0 }).ToList(),
					["ogis"] = o.OGIs.Select(g => new[] { (int)g, ogiJoints.TryGetValue(g, out int n) ? n : 0 }).ToList(),
				};
				if (objectModels.TryGetValue(o.ID, out var models))
				{
					row["models"] = models.Select(m => new Dictionary<string, object> { ["ogi"] = m.Ogi, ["model"] = m.Model }).ToList();
				}
				objects[o.ID.ToString()] = row;
			}

			var scripts = new Dictionary<string, object>();
			foreach (var s in items.OfType<Script>().GroupBy(s => s.ID).Select(g => g.First()))
			{
				var row = new Dictionary<string, object> { ["name"] = s.Name ?? "" };
				if (s.Header != null)
				{
					var pair = s.Header.pairs.FirstOrDefault();
					row["main"] = pair != null ? pair.mainScriptIndex - 1 : -1;
				}
				else if (s.Main != null)
				{
					row["name"] = s.Main.name ?? s.Name ?? "";
					row["start"] = s.Main.StartUnit;
					row["states"] = States(s.Main);
				}
				scripts[s.ID.ToString()] = row;
			}

			string path = Path.Combine(outRoot, "levels", level + ".scripts.json");
			Directory.CreateDirectory(Path.GetDirectoryName(path));
			var sb = new StringBuilder();
			Json.Write(sb, new Dictionary<string, object> { ["objects"] = objects, ["scripts"] = scripts });
			File.WriteAllText(path, sb.ToString());
			return $"{scripts.Count} scripts";
		}

		static List<object> States(Script.MainScript m)
		{
			var states = new List<object>();
			for (var st = m.scriptState1; st != null; st = st.nextState)
			{
				var rules = new List<object>();
				for (var body = st.scriptStateBody; body != null; body = body.nextScriptStateBody)
				{
					var cmds = new List<object>();
					for (var c = body.command; c != null; c = c.nextCommand)
					{
						var cmd = new List<object> { (int)c.VTableIndex };
						cmd.AddRange((c.arguments ?? new List<uint>()).Select(a => (object)a));
						cmds.Add(cmd);
					}
					var cond = body.condition;
					rules.Add(new Dictionary<string, object>
					{
						["cond"] = cond == null ? -1 : (int)cond.VTableIndex,
						["param"] = cond == null ? 0 : (int)cond.Parameter,
						["not"] = cond != null && cond.NotGate,
						["interval"] = cond == null ? 0f : cond.Interval,
						["threshold"] = cond == null ? 0f : cond.Threshold,
						["bits"] = body.bitfield,
						["to"] = (body.bitfield & 0x400) != 0 ? body.scriptStateListIndex : -1,
						["cmds"] = cmds,
					});
				}
				var state = new Dictionary<string, object>
				{
					["bits"] = (int)(ushort)st.bitfield,
					["sub"] = (int)st.scriptIndexOrSlot,
					["slot"] = st.scriptIndexOrSlot != -1 && st.IsSlot,
					["control"] = st.type1 != null,
					["rules"] = rules,
				};
				if (st.type1 != null)
				{
					state["motion"] = Motion(st.type1);
				}
				states.Add(state);
			}
			return states;
		}

		// A "+control" state's motion controller (SupportType1): space, motion type and flag word, plus its
		// parameters by name - bytes[i] is the float slot holding parameter i (the editor's ByteOrderVariables),
		// 0x80 and up meaning "not set". SELECTOR, KEY_INDEX and JOINT_INDEX are ints.
		static readonly string[] s_motionParams =
		{
			"SELECTOR", "KEY_INDEX", "MOVE_SPEED", "TURN_SPEED", "RAWPOS_X", "RAWPOS_Y", "RAWPOS_Z", "PITCH", "YAW", "ROLL",
			"DELAY", "DURATION", "TUMBLE", "SPIN", "TWIST", "SQR_TOLERANCE", "POWER", "DAMPING", "AC_DIST", "DEC_DIST", "BOUNCE",
			"SYNC_UNIT", "JOINT_INDEX",
		};

		static Dictionary<string, object> Motion(SupportType1 t)
		{
			var p = new Dictionary<string, object>();
			for (int i = 0; i < t.bytes.Count && i < s_motionParams.Length; i++)
			{
				int slot = t.bytes[i];
				if (slot < 128 && slot < t.floats.Count)
				{
					p[s_motionParams[i]] = i == 0 || i == 1 || i == 22 ? (object)(int)t.floats[slot] : t.floats[slot];
				}
			}
			return new Dictionary<string, object>
			{
				["space"] = t.Space.ToString(), ["motion"] = t.Motion.ToString(), ["translates"] = t.Translates, ["rotates"] = t.Rotates,
				["yawFaces"] = t.YawFaces, ["keyIsLocal"] = t.KeyIsLocal, ["accel"] = t.AccelFunc.ToString(), ["params"] = p,
			};
		}
	}
}
