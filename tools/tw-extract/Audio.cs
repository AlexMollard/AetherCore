using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Twinsanity;
using Path = System.IO.Path;

namespace TwExtract
{
	// Sound effects (PS2 SPU ADPCM in the .rm2 code section) and streamed music (/CRASH6/MUSIC.MH + .MB)
	// -> 16-bit PCM WAV under assets/audio/. Everything here is ISO-derived and gitignored.
	static class AudioExport
	{
		const int DoSound = 11, SetSound = 86, BeginMusic = 88, EndContextMusic = 89, CaDoSound = 545, SoundProxy = 622;
		static readonly Dictionary<int, string> SoundCommands = new Dictionary<int, string>
		{
			[DoSound] = "DoSound", [SetSound] = "SetSound", [BeginMusic] = "BeginMusic",
			[EndContextMusic] = "EndContextMusic", [CaDoSound] = "CA_DoSound", [SoundProxy] = "CharacterSoundProxy",
		};

		// Code section 10: sub-section 6 holds the language-neutral effects, 7 the English voice bank.
		// assets/audio/sfx/<level>/<id>.wav plus <level>/sounds.json: every effect, which object lists it in its
		// sound slots, and every script command that names a sound or music track (raw arguments).
		public static string Write(TwinsFile file, string level, string outDir, HashSet<int> musicTracks)
		{
			if (!file.ContainsItem(10))
			{
				return "no code section";
			}
			var code = file.GetItem<TwinsSection>(10);
			string dir = Path.Combine(outDir, "audio", "sfx", level);
			Directory.CreateDirectory(dir);
			var effects = new List<object>();
			foreach (var (sub, bank) in new[] { (6u, ""), (7u, "eng_") })
			{
				if (code.ContainsItem(sub))
				{
					Bank(code.GetItem<TwinsSection>(sub), dir, bank, effects);
				}
			}


			var items = Gfx.Items(file).ToList();
			var scripts = items.OfType<Script>().GroupBy(s => s.ID).ToDictionary(g => g.Key, g => g.First());
			var owners = new Dictionary<uint, List<string>>(); // script id -> "Object.Slot"
			var slotNames = Enum.GetNames(typeof(DefaultEnums.CharacterGameObjectScriptOrder));
			var objects = new List<object>();
			foreach (var obj in items.OfType<GameObject>())
			{
				for (int slot = 0; slot < obj.Scripts.Count; slot++)
				{
					string owner = $"{obj.Name}.{(slot < slotNames.Length ? slotNames[slot] : slot.ToString())}";
					if (!owners.TryGetValue(obj.Scripts[slot], out var list))
					{
						owners[obj.Scripts[slot]] = list = new List<string>();
					}
					list.Add(owner);
				}
				if (obj.Sounds.Count > 0 || obj.cSounds.Count > 0)
				{
					objects.Add(new Dictionary<string, object> { ["object"] = obj.Name, ["id"] = obj.ID, ["sounds"] = obj.Sounds.Cast<object>().ToList(), ["instanceSounds"] = obj.cSounds.Cast<object>().ToList() });
				}
			}
			var commands = new List<object>();
			foreach (var script in scripts.Values)
			{
				var mains = new List<Script.MainScript>();
				if (script.Main != null)
				{
					mains.Add(script.Main);
				}
				var users = new List<string>();
				if (owners.TryGetValue(script.ID, out var direct))
				{
					users.AddRange(direct);
				}
				// Header scripts (the ones behaviour slots name) point at their main scripts.
				foreach (var hs in scripts.Values.Where(h => h.Header != null && h.Header.pairs.Any(p => p.mainScriptIndex - 1 == script.ID)))
				{
					if (owners.TryGetValue(hs.ID, out var via))
					{
						users.AddRange(via);
					}
				}
				foreach (var main in mains)
				{
					int stateIndex = 0;
					for (var state = main.scriptState1; state != null; state = state.nextState, stateIndex++)
					{
						for (var body = state.scriptStateBody; body != null; body = body.nextScriptStateBody)
						{
							for (var cmd = body.command; cmd != null; cmd = cmd.nextCommand)
							{
								int id = cmd.internalIndex & 0xFFFF;
								if (!SoundCommands.TryGetValue(id, out string name))
								{
									continue;
								}
								commands.Add(new Dictionary<string, object>
								{
									["script"] = script.ID, ["name"] = script.Name, ["state"] = stateIndex, ["cmd"] = name,
									["args"] = (cmd.arguments ?? new List<uint>()).Cast<object>().ToList(), ["users"] = users.Distinct().ToList(),
								});
							}
						}
					}
				}
			}
			// The stream a level plays is instance data, not a script argument: BeginMusic names a
			// context (the DJ's 1, the ambience actor's 17) and the placed actor's params carry the
			// MUSIC.MH track - act_DJ params[0] (Beach: 27, the title theme), act_GLOBAL_AMBIENT_SOUND_*
			// params[2] (Beach: 89, the surf bed). Both confirmed on the rig by correlating a PCSX2
			// capture against every track (logs/audio/README.md).
			var streams = new List<object>();
			var names = items.OfType<GameObject>().GroupBy(o => o.ID).ToDictionary(g => g.Key, g => g.First().Name);
			for (uint layer = 0; layer < 8; layer++)
			{
				if (!file.ContainsItem(layer) || !(file.GetItem<TwinsItem>(layer) is TwinsSection section) || !section.ContainsItem(6)
					|| !(section.GetItem<TwinsItem>(6) is TwinsSection placed))
				{
					continue;
				}
				foreach (var ins in placed.Records.OfType<Instance>())
				{
					string name = names.TryGetValue(ins.ObjectID, out string n) ? n : "";
					int slot = name.EndsWith("act_DJ") ? 0 : name.Contains("act_GLOBAL_AMBIENT_SOUND") ? 2 : -1;
					if (slot < 0 || ins.UnkI323.Count <= slot)
					{
						continue;
					}
					int track = (int)ins.UnkI323[slot];
					musicTracks?.Add(track);
					streams.Add(new Dictionary<string, object> { ["object"] = name, ["kind"] = slot == 0 ? "music" : "ambience", ["track"] = track });
				}
			}
			var sb = new StringBuilder();
			Json.Write(sb, new Dictionary<string, object> { ["effects"] = effects, ["objects"] = objects, ["commands"] = commands, ["streams"] = streams });
			File.WriteAllText(Path.Combine(dir, "sounds.json"), sb.ToString());
			return $"{effects.Count} sounds, {commands.Count} sound commands, streams {string.Join(",", streams.Select(s => ((Dictionary<string, object>)s)["track"]))}";
		}

		// Startup/Frontend.bin is a bare SE section: the menu sounds -> assets/audio/sfx/Startup/Frontend/<id>.wav.
		public static string Frontend(string path, string outDir)
		{
			var file = new TwinsFile();
			file.LoadFile(path, TwinsFile.FileType.Frontend);
			string dir = Path.Combine(outDir, "audio", "sfx", "Startup", "Frontend");
			Directory.CreateDirectory(dir);
			var effects = new List<object>();
			Bank(file.GetItem<TwinsSection>(3), dir, "", effects);
			var sb = new StringBuilder();
			Json.Write(sb, new Dictionary<string, object> { ["effects"] = effects });
			File.WriteAllText(Path.Combine(dir, "sounds.json"), sb.ToString());
			return $"{effects.Count} frontend sounds";
		}

		static void Bank(TwinsSection section, string dir, string prefix, List<object> effects)
		{
			foreach (var se in section.Records.OfType<SoundEffect>())
			{
				var raw = new byte[se.SoundSize];
				Array.Copy(section.ExtraData, se.SoundOffset, raw, 0, se.SoundSize);
				int freq;
				try
				{
					freq = se.Freq;
				}
				catch (ArgumentException)
				{
					freq = 22050;
				}
				var pcm = ADPCM.ToPCMMono(raw, raw.Length);
				File.WriteAllBytes(Path.Combine(dir, $"{prefix}{se.ID}.wav"), RIFF.SaveRiff(pcm, 1, freq));
				effects.Add(new Dictionary<string, object>
				{
					["id"] = se.ID, ["file"] = $"{prefix}{se.ID}.wav", ["rate"] = freq, ["seconds"] = Math.Round(pcm.Length / 2.0 / freq, 3),
					["loop"] = LoopFlag(raw), ["flag"] = (int)se.UnkFlag, ["params"] = new object[] { (int)se.Param1, (int)se.Param2, (int)se.Param3, (int)se.Param4 },
				});
			}
		}

		// SPU ADPCM block flags: bit 1 (0x02) on the last block's flag byte = loop, bit 2 (0x04) = loop start.
		static bool LoopFlag(byte[] raw)
		{
			for (int p = 0; p + 16 <= raw.Length; p += 16)
			{
				if ((raw[p + 1] & 0x02) != 0 && (raw[p + 1] & 0x01) != 0)
				{
					return true;
				}
			}
			return false;
		}

		// /CRASH6/MUSIC.MH: count, interleave, then per track {type, size, offset, rate, unk}. Type 1 = stereo
		// ADPCM interleaved in `interleave`-byte blocks, 0 = mono "MSVp" with a 0x30 header, 2 = no track.
		// assets/audio/music/track_<n>.wav. ENGLISH.MH/.MB (cutscene speech, same layout) -> audio/voice/track_<n>.wav.
		public static string Music(Disc disc, string outDir, IEnumerable<int> tracks, string bank = "MUSIC")
		{
			var mh = disc.ReadIsoFile($"/CRASH6/{bank}.MH");
			int count = BitConverter.ToInt32(mh, 0), interleave = BitConverter.ToInt32(mh, 4);
			string dir = Path.Combine(outDir, "audio", bank == "MUSIC" ? "music" : "voice");
			Directory.CreateDirectory(dir);
			var done = new List<int>();
			foreach (int t in tracks.Distinct().OrderBy(t => t))
			{
				if (t < 0 || t >= count)
				{
					continue;
				}
				int p = 8 + 20 * t, type = BitConverter.ToInt32(mh, p), size = BitConverter.ToInt32(mh, p + 4), rate = BitConverter.ToInt32(mh, p + 12);
				uint offset = BitConverter.ToUInt32(mh, p + 8);
				if (type == 2 || size <= 0)
				{
					continue;
				}
				var raw = disc.ReadIsoRange($"/CRASH6/{bank}.MB", offset, size);
				// Unassigned slots all share one mono MSVp stream named "undefined": a 335 Hz test tone, not music.
				if (type == 0 && Encoding.ASCII.GetString(raw, 0x20, 0x10).TrimEnd('\0') == "undefined")
				{
					continue;
				}
				byte[] wav = type == 1
					? RIFF.SaveRiff(ADPCM.ToPCMStereo(raw, raw.Length, interleave), 2, rate)
					: RIFF.SaveRiff(ADPCM.ToPCMMono(raw.Skip(0x30).ToArray(), raw.Length - 0x30), 1, rate);
				File.WriteAllBytes(Path.Combine(dir, $"track_{t}.wav"), wav);
				done.Add(t);
			}
			return $"{bank} tracks {string.Join(",", done)}";
		}
	}
}
