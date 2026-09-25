// tw-extract - Crash Twinsanity (PAL, SLES-52568) disc -> glTF + PNG for projects/Twinsanity/assets.
//
//   tw-extract --iso <original.iso> [--out projects/Twinsanity/assets] [--cache <dir>] [--only <substring>]
//
// Output (all gitignored - ISO-derived content never enters the repo, see docs/twinsanity-editor.md).
// Models live under models/ because that is what AssetPacker bake-all (and the build's auto-bake) bakes.
//   textures/<hash>.png                                every decoded texture, content-addressed and shared
//   models/scenery/<Area>/<Level>/<chunk>/<chunk>.gltf  a chunk's static scenery, world space, one primitive per material
//   models/scenery/.../<chunk>_sky.gltf                  the chunk's skydome
//   models/scenery/.../<chunk>_dynamic.gltf              animated scenery pieces at their initial transforms
//   models/objects/<Object>/<Object>[_<n>].gltf          each game object's graphics: skeleton, skin, rigid parts
//   models/collision/<Area>/<Level>/<chunk>/*.gltf       collision pieces (see LevelExport)
//   levels/<Area>/<Level>/<chunk>.level.json             everything a runtime needs to assemble the chunk
//   images/<path>/<stem>_NN.png                          gallery / loading-screen pictures (.psm)
//   ui/icons/Icons_NN.png, Decal_00.png                  HUD / menu sprites (Startup/Icons.psm, Decal.ptc), GS-brightened
//   ui/titles/<Language>/<Level>_00.png                  level title badges (Language/Titles), GS-brightened
//   ui/text/<Language>.txt                               menu strings (Language/Code)
//   ui/fonts/<font>_NN.png, <font>/<code>.png, <font>.font.json   bitmap fonts (Startup/Fonts/*.psf): pages, glyphs, metrics
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text;
using Twinsanity;
using Path = System.IO.Path;

namespace TwExtract
{
	static class Program
	{
		static string s_out;
		static TextureStore s_textures;
		// objects/<name>: content hash -> file stem already written, so a model shared by many levels is
		// written once and every level can still name it.
		static readonly Dictionary<string, Dictionary<string, string>> s_objectFiles = new Dictionary<string, Dictionary<string, string>>();
		// Object IDs are global to the game (DefaultEnums.ObjectID); crates and wumpa are placed by every level
		// but defined once, in Startup. ID -> { name, model } across every archive, first definition wins.
		static readonly SortedDictionary<uint, Dictionary<string, object>> s_objectTable = new SortedDictionary<uint, Dictionary<string, object>>();
		static int s_scenery, s_objects, s_images, s_failed;

		static int Main(string[] args)
		{
			string iso = null, only = null, cache = null;
			s_out = Path.Combine("projects", "Twinsanity", "assets");
			for (int i = 0; i < args.Length; i++)
			{
				string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value");
				switch (args[i])
				{
					case "--iso": iso = Next(); break;
					case "--out": s_out = Next(); break;
					case "--cache": cache = Next(); break;
					case "--only": only = Next(); break;
					default:
						Console.Error.WriteLine("usage: tw-extract --iso <original.iso> [--out <assets dir>] [--cache <dir>] [--only <substring>]");
						return 2;
				}
			}
			if (iso == null || !File.Exists(iso))
			{
				Console.Error.WriteLine(iso == null ? "tw-extract: --iso is required (the untouched PAL disc)." : $"tw-extract: no such file '{iso}'.");
				return 2;
			}

			using (var disc = new Disc(iso))
			{
				if (disc.Crc != Disc.OriginalCrc)
				{
					Console.Error.WriteLine($"tw-extract: disc CRC {disc.Crc} is not the original PAL release ({Disc.OriginalCrc}); refusing a modded or foreign image.");
					return 1;
				}
				cache = cache ?? Path.Combine(Path.GetTempPath(), "tw-extract", disc.Crc);
				Directory.CreateDirectory(s_out);
				s_textures = new TextureStore(s_out);

				var stdout = Console.Out;
				foreach (var entry in disc.Entries)
				{
					string name = entry.Name.Replace('\\', '/');
					string ext = Path.GetExtension(name).ToLowerInvariant();
					bool text = ext == ".txt" && name.StartsWith("Language/Code/", StringComparison.OrdinalIgnoreCase);
					if ((ext != ".sm2" && ext != ".rm2" && ext != ".psm" && ext != ".psf" && ext != ".ptc" && !text) || (only != null && name.IndexOf(only, StringComparison.OrdinalIgnoreCase) < 0))
					{
						continue;
					}
					string local = Path.Combine(cache, name);
					if (!File.Exists(local) || new FileInfo(local).Length != entry.Size)
					{
						Directory.CreateDirectory(Path.GetDirectoryName(local));
						File.WriteAllBytes(local, disc.Read(entry));
					}
					try
					{
						Console.SetOut(TextWriter.Null); // the library prints load chatter
						string result = ext == ".sm2" ? Scenery(local, name) : ext == ".rm2" ? Rm2(local, name) : ext == ".psf" ? Font(local, name) : text ? Strings(local, name) : Images(local, name);
						Console.SetOut(stdout);
						Console.WriteLine($"{name}: {result}");
					}
					catch (Exception e)
					{
						Console.SetOut(stdout);
						s_failed++;
						Console.Error.WriteLine($"{name}: FAILED {e.GetType().Name}: {e.Message}");
					}
				}
			}
			// Only a whole-disc run sees every definition; a partial table would silently lose objects.
			if (only == null)
			{
				var sb = new StringBuilder();
				Json.Write(sb, s_objectTable.ToDictionary(kv => kv.Key.ToString(), kv => (object)kv.Value));
				Directory.CreateDirectory(Path.Combine(s_out, "levels"));
				File.WriteAllText(Path.Combine(s_out, "levels", "objects.json"), sb.ToString());
			}
			Console.WriteLine($"done: {s_scenery} scenery glTF, {s_objects} object glTF, {s_images} images, {s_textures.Written} textures ({s_textures.Undecodable} undecodable), {s_failed} files failed");
			return s_failed == 0 ? 0 : 1;
		}

		// "Levels/Earth/Hub/Beach.sm2" -> "Earth/Hub/Beach"
		static string LevelPath(string archiveName)
		{
			string noExt = archiveName.Substring(0, archiveName.Length - Path.GetExtension(archiveName).Length);
			return noExt.StartsWith("Levels/", StringComparison.OrdinalIgnoreCase) ? noExt.Substring(7) : noExt;
		}

		static string Rel(string fromDir, string toDir) =>
		        Uri.UnescapeDataString(new Uri(Path.GetFullPath(fromDir) + Path.DirectorySeparatorChar).MakeRelativeUri(new Uri(Path.GetFullPath(toDir) + Path.DirectorySeparatorChar)).ToString()).TrimEnd('/');

		static Export NewExport(Gfx gfx, string gltfDir) => new Export(gfx, s_textures, Rel(gltfDir, Path.Combine(s_out, "textures")));

		// Object models are not prelit scenery: they are lit at run time by the level's
		// light records, so their materials get {"object_lit":true} instead.
		static Export NewObjectExport(Gfx gfx, string gltfDir) => new Export(gfx, s_textures, Rel(gltfDir, Path.Combine(s_out, "textures")), isObject: true);

		static TwinsFile Load(string path, TwinsFile.FileType type)
		{
			var file = new TwinsFile();
			file.LoadFile(path, type);
			return file;
		}

		// ---- scenery (.sm2) -------------------------------------------------------------------------

		static string Scenery(string path, string archiveName)
		{
			var file = Load(path, TwinsFile.FileType.SM2);
			var gfx = new Gfx(file, 6);
			string level = LevelPath(archiveName), stem = Path.GetFileName(level);
			string dir = Path.Combine(s_out, "models", "scenery", level);
			var notes = new List<string>();

			if (file.ContainsItem(0) && file.GetItem<TwinsItem>(0) is SceneryData scenery && scenery.SceneryRoot != null)
			{
				var ex = NewExport(gfx, dir);
				var prims = new Dictionary<(uint mat, int layer), Prim>();
				int instances = 0;
				void Leaf(SceneryData.SceneryModelStruct leaf)
				{
					if (leaf?.Models == null)
					{
						return;
					}
					foreach (var sub in leaf.Models)
					{
						// "Special" instances point at a LodModel; take its most detailed level.
						uint rigidId = sub.isSpecial ? (gfx.Lods.TryGetValue(sub.ModelID, out var lod) && lod.LODModelIDs.Length > 0 ? lod.LODModelIDs[0] : 0xDDDDDDDD) : sub.ModelID;
						if (!gfx.Rigids.TryGetValue(rigidId, out var rigid) || !gfx.Models.TryGetValue(rigid.MeshID, out var model))
						{
							continue;
						}
						AppendRigid(ex, prims, rigid, model, InstanceMatrix(sub.ModelMatrix));
						instances++;
					}
				}
				void Branch(SceneryData.SceneryStruct node)
				{
					Leaf(node.Model);
					foreach (var link in node.Links ?? Array.Empty<object>())
					{
						if (link is SceneryData.SceneryStruct s)
						{
							Branch(s);
						}
						else if (link is SceneryData.SceneryModelStruct m)
						{
							Leaf(m);
						}
					}
				}
				Branch(scenery.SceneryRoot);
				if (SingleMeshNode(ex, stem, prims))
				{
					ex.Save(Path.Combine(dir, stem + ".gltf"));
					s_scenery++;
					notes.Add($"{instances} instances, {prims.Count} materials");
				}

				if (scenery.SkydomeID != 0 && gfx.Skydomes.TryGetValue(scenery.SkydomeID, out var sky))
				{
					var skyEx = NewExport(gfx, dir);
					var skyPrims = new Dictionary<(uint mat, int layer), Prim>();
					foreach (uint id in sky.MeshIDs)
					{
						if (gfx.Rigids.TryGetValue(id, out var rigid) && gfx.Models.TryGetValue(rigid.MeshID, out var model))
						{
							AppendRigid(skyEx, skyPrims, rigid, model, Matrix4x4.Identity);
						}
					}
					if (SingleMeshNode(skyEx, stem + "_sky", skyPrims))
					{
						skyEx.Save(Path.Combine(dir, stem + "_sky.gltf"));
						s_scenery++;
						notes.Add("sky");
					}
				}
			}

			if (file.ContainsItem(4) && file.GetItem<TwinsItem>(4) is DynamicSceneryData dynamic && dynamic.Models != null)
			{
				var ex = NewExport(gfx, dir);
				int placed = 0;
				foreach (var m in dynamic.Models)
				{
					if (!gfx.Rigids.TryGetValue(m.ModelID, out var rigid) || !gfx.Models.TryGetValue(rigid.MeshID, out var model))
					{
						continue;
					}
					var prims = new Dictionary<(uint mat, int layer), Prim>();
					AppendRigid(ex, prims, rigid, model, Matrix4x4.Identity);
					int mesh = AddMesh(ex, $"dynamic{placed}", prims);
					if (mesh < 0)
					{
						continue;
					}
					// Initial transform from the animation's first key. [UNVERIFIED] quaternion layout (x, y, z, w).
					var q = new Quaternion(m.WorldRotation.X, m.WorldRotation.Y, m.WorldRotation.Z, m.WorldRotation.W);
					q = q.LengthSquared() > 1e-8f ? Quaternion.Normalize(Space.Mirror(q)) : Quaternion.Identity;
					var t = Space.Mirror(new Vector3(m.WorldPosition.X, m.WorldPosition.Y, m.WorldPosition.Z));
					ex.Gltf.SceneRoots.Add(ex.Gltf.AddNode(new Dictionary<string, object>
					{
						["name"] = $"dynamic{placed}",
						["mesh"] = mesh,
						["translation"] = new[] { t.X, t.Y, t.Z },
						["rotation"] = new[] { q.X, q.Y, q.Z, q.W },
					}));
					placed++;
				}
				if (placed > 0)
				{
					ex.Save(Path.Combine(dir, stem + "_dynamic.gltf"));
					s_scenery++;
					notes.Add($"{placed} dynamic");
				}
			}
			return notes.Count > 0 ? string.Join(", ", notes) : "no scenery";
		}

		// A scenery instance's 4x4: rows are the X/Y/Z basis and the translation, applied p' = p * M.
		static Matrix4x4 InstanceMatrix(Pos[] m) => new Matrix4x4(
		        m[0].X, m[0].Y, m[0].Z, 0,
		        m[1].X, m[1].Y, m[1].Z, 0,
		        m[2].X, m[2].Y, m[2].Z, 0,
		        m[3].X, m[3].Y, m[3].Z, 1);

		// The PS2 draws every distinct texture-mapped shader record of a material over the same
		// geometry (base texture, then blended overlays - the sea surface, the cloud sheet), so
		// each layer becomes its own primitive reusing the same vertices with its own material.
		static void AppendRigid(Export ex, Dictionary<(uint mat, int layer), Prim> prims, RigidModel rigid, Model model, Matrix4x4 xf)
		{
			for (int k = 0; k < model.SubModels.Count; k++)
			{
				uint mat = k < rigid.MaterialIDs.Length ? rigid.MaterialIDs[k] : rigid.MaterialIDs.LastOrDefault();
				for (int layer = 0; layer < ex.LayerCount(mat); layer++)
				{
					if (!prims.TryGetValue((mat, layer), out var p))
					{
						prims[(mat, layer)] = p = new Prim();
					}
					Meshes.AppendModel(p, model.SubModels[k], xf);
				}
			}
		}

		static int AddMesh(Export ex, string name, Dictionary<(uint mat, int layer), Prim> prims)
		{
			foreach (var p in prims.Values)
			{
				Meshes.FillNormals(p);
			}
			// Several game materials can resolve to one glTF material; merge them so each becomes one entity.
			var merged = new Dictionary<int, Prim>();
			foreach (var kv in prims.Where(kv => kv.Value.Idx.Count > 0))
			{
				int material = ex.Material(kv.Key.mat, kv.Value.Col.Count > 0, kv.Key.layer);
				if (material < 0)
				{
					continue;
				}
				if (merged.TryGetValue(material, out var into))
				{
					into.Append(kv.Value);
				}
				else
				{
					merged[material] = kv.Value;
				}
			}
			return ex.Gltf.AddMesh(name, merged.Select(kv => (kv.Value, kv.Key)).ToList());
		}

		static bool SingleMeshNode(Export ex, string name, Dictionary<(uint mat, int layer), Prim> prims)
		{
			int mesh = AddMesh(ex, name, prims);
			if (mesh < 0)
			{
				return false;
			}
			ex.Gltf.SceneRoots.Add(ex.Gltf.AddNode(new Dictionary<string, object> { ["name"] = name, ["mesh"] = mesh }));
			return true;
		}

		// ---- objects + level (.rm2) -----------------------------------------------------------------

		static string Rm2(string path, string archiveName)
		{
			var file = Load(path, TwinsFile.FileType.RM2);
			var gfx = new Gfx(file, 11);
			string objects = Objects(file, gfx, archiveName, out var models);
			string particles = "";
			// Startup/Default.rm2 holds the shared particle bank: its three texture pages are
			// what the runtime's billboard emitters sample. Everything else re-exports the
			// same bank per level, so only write it once.
			if (archiveName.EndsWith("Startup/Default.rm2", StringComparison.OrdinalIgnoreCase))
			{
				particles = ", " + ParticlePages(file, gfx);
			}
			return objects + particles + ", " + LevelExport.Write(file, Path.ChangeExtension(path, ".sm2"), LevelPath(archiveName), s_out, models);
		}

		// ParticleData's three texture pages -> assets/particles/particle_page_<n>.png. The
		// pages are drawn point-sampled at their native texels; no 2x brighten (their own
		// colour/alpha gradients supply the gain).
		static string ParticlePages(TwinsFile file, Gfx gfx)
		{
			var pd = Gfx.Items(file).OfType<ParticleData>().FirstOrDefault() ?? throw new InvalidDataException("no ParticleData in Default.rm2");
			string dir = Path.Combine(s_out, "particles");
			Directory.CreateDirectory(dir);
			uint[] ids = { pd.ParticleTextureID_1, pd.ParticleTextureID_2, pd.ParticleTextureID_3 };
			int saved = 0;
			var pages = new List<object>();
			for (int i = 0; i < ids.Length; i++)
			{
				if (!gfx.Textures.TryGetValue(ids[i], out var tex))
				{
					continue;
				}
				var rgba = Pixels.Decode(tex) ?? throw new InvalidDataException($"particle page {i}: undecodable {tex.PixelFormat}");
				string name = $"particle_page_{i}.png";
				Pixels.SavePng(Path.Combine(dir, name), tex.Width, tex.Height, rgba);
				pages.Add(new Dictionary<string, object> { ["page"] = i, ["file"] = name, ["width"] = tex.Width, ["height"] = tex.Height });
				saved++;
			}
			return $"{saved} particle pages";
		}

		// models: object ID -> project:// path of the object's first graphics set, whether written now or by
		// an earlier level.
		static string Objects(TwinsFile file, Gfx gfx, string archiveName, out Dictionary<uint, string> models)
		{
			models = new Dictionary<uint, string>();
			var items = Gfx.Items(file).ToList();
			var ogis = new Dictionary<uint, GraphicsInfo>();
			foreach (var gi in items.OfType<GraphicsInfo>())
			{
				ogis[gi.ID] = gi;
			}
			var animations = new Dictionary<uint, Animation>();
			foreach (var a in items.OfType<Animation>())
			{
				animations[a.ID] = a;
			}
			var scripts = new Dictionary<uint, Script>();
			foreach (var s in items.OfType<Script>())
			{
				scripts[s.ID] = s;
			}
			int written = 0, shared = 0;
			foreach (var obj in items.OfType<GameObject>())
			{
				var used = obj.OGIs.Where(id => ogis.ContainsKey(id)).Distinct().ToList();
				// Clips are named by the object's animation slot, which is what its scripts' DoAnim commands name.
				var clips = obj.Anims.Select((id, slot) => (Id: id, Slot: slot))
					.Where(a => animations.ContainsKey(a.Id))
					.Select(a => ($"a{a.Slot:D3}", animations[a.Id])).ToList();
				for (int k = 0; k < used.Count; k++)
				{
					string name = Safe(obj.Name) + (used.Count > 1 ? $"_{k}" : "");
					string dir = Path.Combine(s_out, "models", "objects", Safe(obj.Name));
					var ex = NewObjectExport(gfx, dir);
					var hulls = new List<(string Suffix, Prim Hull)>();
					if (!BuildObject(ex, gfx, ogis[used[k]], name, clips, hulls))
					{
						continue;
					}
					// Write once per distinct content; a different model under a taken name gets the level's name.
					string hash = ex.Gltf.ContentHash();
					if (!s_objectFiles.TryGetValue(name, out var seen))
					{
						s_objectFiles[name] = seen = new Dictionary<string, string>();
					}
					if (!seen.TryGetValue(hash, out string fileName))
					{
						fileName = seen.Count == 0 ? name : $"{name}@{Path.GetFileName(LevelPath(archiveName))}";
						seen[hash] = fileName;
						ex.Save(Path.Combine(dir, fileName + ".gltf"));
						SaveHulls(dir, fileName, hulls);
						written++;
						s_objects++;
					}
					else
					{
						shared++;
					}
					if (!models.ContainsKey(obj.ID))
					{
						models[obj.ID] = VfsPath(Path.Combine(dir, fileName + ".gltf"));
					}
				}
				// Multi-OGI objects animate by swapping model states (crates do this instead of
			// skeletal clips - they are single-joint). Record which OGI id each _k file is so
			// runtime code can follow the original's state sequences.
				if (used.Count > 1)
				{
					var ogiMap = new List<object>();
					for (int k = 0; k < used.Count; k++)
					{
						ogiMap.Add(new Dictionary<string, object> { ["k"] = k, ["ogi"] = used[k] });
					}
					var ob = new StringBuilder();
					Json.Write(ob, new Dictionary<string, object> { ["ogis"] = ogiMap });
					string objectDir = Path.Combine(s_out, "models", "objects", Safe(obj.Name));
					Directory.CreateDirectory(objectDir);
					File.WriteAllText(Path.Combine(objectDir, Safe(obj.Name) + ".ogis.json"), ob.ToString());
				}
				// Characters (the behaviour slots past OnPhysicsCollision): which clips each state plays.
				if (obj.Scripts.Count > 12 && clips.Count > 0)
				{
					var states = AnimExport.CharacterStates(obj, scripts);
					if (states.Count > 0)
					{
						var sb = new StringBuilder();
						Json.Write(sb, new Dictionary<string, object> { ["fps"] = AnimExport.Fps, ["states"] = states });
						string objectDir = Path.Combine(s_out, "models", "objects", Safe(obj.Name));
						Directory.CreateDirectory(objectDir);
						File.WriteAllText(Path.Combine(objectDir, Safe(obj.Name) + ".states.json"), sb.ToString());
					}
				}
				if (!s_objectTable.ContainsKey(obj.ID))
				{
					var row = new Dictionary<string, object> { ["name"] = Safe(obj.Name) };
					if (models.TryGetValue(obj.ID, out string model))
					{
						row["model"] = model;
					}
					s_objectTable[obj.ID] = row;
				}
			}
			return $"{written} objects written, {shared} already extracted";
		}

		// <model>.hulls.json next to the model: {"hulls": [{"rest": gltf, "clips": {"a001": gltf}}]}, each gltf
		// one convex hull in model space (see AddHulls) for a runtime convex-hull body.
		static void SaveHulls(string dir, string fileName, List<(string Suffix, Prim Hull)> hulls)
		{
			if (hulls.Count == 0)
			{
				return;
			}
			var rows = new List<object>();
			Dictionary<string, object> clipsOf = null;
			foreach (var (suffix, prim) in hulls)
			{
				var g = new Gltf();
				int mesh = g.AddMesh("hull", new[] { (prim, -1) });
				g.SceneRoots.Add(g.AddNode(new Dictionary<string, object> { ["name"] = "hull", ["mesh"] = mesh }));
				string path = Path.Combine(dir, $"{fileName}_{suffix}.gltf");
				g.Save(path);
				int clip = suffix.IndexOf('_');
				if (clip < 0)
				{
					clipsOf = new Dictionary<string, object>();
					rows.Add(new Dictionary<string, object> { ["rest"] = VfsPath(path), ["clips"] = clipsOf });
				}
				else
				{
					clipsOf[suffix.Substring(clip + 1)] = VfsPath(path);
				}
			}
			var sb = new StringBuilder();
			Json.Write(sb, new Dictionary<string, object> { ["hulls"] = rows });
			File.WriteAllText(Path.Combine(dir, fileName + ".hulls.json"), sb.ToString());
		}

		public static string VfsPath(string underAssets) => "project://assets/" + Rel(s_out, Path.GetDirectoryName(underAssets)) + "/" + Path.GetFileName(underAssets);

		// hulls receives the OGI's collision hulls (GI_CollisionData) as model-space triangle lists: suffix
		// "hull<k>" at the rest pose and "hull<k>_<clip>" for the pose a rigid prop holds at the end of a clip
		// that moves the hull's joint.
		static bool BuildObject(Export ex, Gfx gfx, GraphicsInfo gi, string name, List<(string, Animation)> clips, List<(string Suffix, Prim Hull)> hulls)
		{
			var g = ex.Gltf;
			var joints = gi.Joints ?? Array.Empty<GraphicsInfo.Joint>();
			var slot = new Dictionary<uint, int>(); // joint index -> position in joints[]
			for (int i = 0; i < joints.Length; i++)
			{
				slot[joints[i].JointIndex] = i;
			}
			var world = new Matrix4x4?[joints.Length];
			var nodes = new int[joints.Length];
			Matrix4x4 World(int i)
			{
				if (world[i] is Matrix4x4 w)
				{
					return w;
				}
				var (t, q) = JointLocal(joints[i]);
				var local = Matrix4x4.CreateFromQuaternion(q) * Matrix4x4.CreateTranslation(t);
				bool root = i == 0 || !slot.TryGetValue(joints[i].ParentJointIndex, out int parent) || parent == i;
				world[i] = root ? local : local * World(slot[joints[i].ParentJointIndex]);
				return world[i].Value;
			}
			for (int i = 0; i < joints.Length; i++)
			{
				var (t, q) = JointLocal(joints[i]);
				nodes[i] = g.AddNode(new Dictionary<string, object>
				{
					["name"] = $"joint{joints[i].JointIndex}",
					["translation"] = new[] { t.X, t.Y, t.Z },
					["rotation"] = new[] { q.X, q.Y, q.Z, q.W },
				});
			}
			for (int i = 0; i < joints.Length; i++)
			{
				World(i);
				bool root = i == 0 || !slot.TryGetValue(joints[i].ParentJointIndex, out int parent) || parent == i;
				if (root)
				{
					g.SceneRoots.Add(nodes[i]);
				}
				else
				{
					Gltf.AddChild(g.Nodes[nodes[slot[joints[i].ParentJointIndex]]], nodes[i]);
				}
			}
			var endLocals = new Dictionary<string, Matrix4x4?[]>();
			if (joints.Length > 1 && clips.Count > 0)
			{
				AnimExport.Add(g, joints, nodes, clips, endLocals);
			}
			AddHulls(gi, joints, slot, World, endLocals, hulls);

			bool any = false;
			int partCount = 0;
			// Rigid parts ride a joint: vertices are joint-local, so the mesh node is a child of that joint.
			foreach (var link in gi.ModelIDs.Values)
			{
				if (!gfx.Rigids.TryGetValue(link.ModelID, out var rigid) || !gfx.Models.TryGetValue(rigid.MeshID, out var model))
				{
					continue;
				}
				var prims = new Dictionary<(uint mat, int layer), Prim>();
				AppendRigid(ex, prims, rigid, model, Matrix4x4.Identity);
				// Names carry no record IDs: those differ per level and would defeat the cross-level dedupe.
				string part = $"part{partCount++}_joint{link.JointIndex}";
				int mesh = AddMesh(ex, $"{name}_{part}", prims);
				if (mesh < 0)
				{
					continue;
				}
				int node = g.AddNode(new Dictionary<string, object> { ["name"] = part, ["mesh"] = mesh });
				if (slot.TryGetValue(link.JointIndex, out int j))
				{
					Gltf.AddChild(g.Nodes[nodes[j]], node);
				}
				else
				{
					g.SceneRoots.Add(node);
				}
				any = true;
			}

			// Skin + blend skin: bind-pose model-space vertices weighted to up to three joints.
			// Skins are characters: single layer (multi-layer records only occur on scenery).
			var skinPrims = new Dictionary<(uint mat, int layer), Prim>();
			if (gi.SkinID != 0 && gfx.Skins.TryGetValue(gi.SkinID, out var skin))
			{
				foreach (var sub in skin.SubModels)
				{
					AppendSkinned(Get(skinPrims, sub.MaterialID), sub.Vertexes.Select(v => (v.X, v.Y, v.Z, v.U, v.V, v.Joint.JointIndex1, v.Joint.JointIndex2, v.Joint.JointIndex3, v.Joint.Weight1, v.Joint.Weight2, v.Joint.Weight3, v.Conn)).ToList(), slot);
				}
			}
			if (gi.BlendSkinID != 0 && gfx.BlendSkins.TryGetValue(gi.BlendSkinID, out var blend))
			{
				foreach (var model in blend.Models)
				{
					foreach (var sub in model.SubModels)
					{
						AppendSkinned(Get(skinPrims, model.MaterialID), sub.Vertexes.Select(v => (v.X, v.Y, v.Z, v.U, v.V, v.Joint.JointIndex1, v.Joint.JointIndex2, v.Joint.JointIndex3, v.Joint.Weight1, v.Joint.Weight2, v.Joint.Weight3, v.Conn)).ToList(), slot);
					}
				}
			}
			if (skinPrims.Count > 0 && joints.Length > 0)
			{
				int mesh = AddMesh(ex, $"{name}_skin", skinPrims);
				if (mesh >= 0)
				{
					var ibm = new float[joints.Length * 16];
					for (int i = 0; i < joints.Length; i++)
					{
						Matrix4x4.Invert(world[i].Value, out var inv);
						// System.Numerics is row-vector, so its row-major layout is glTF's column-major one.
						float[] m = { inv.M11, inv.M12, inv.M13, inv.M14, inv.M21, inv.M22, inv.M23, inv.M24, inv.M31, inv.M32, inv.M33, inv.M34, inv.M41, inv.M42, inv.M43, inv.M44 };
						Array.Copy(m, 0, ibm, i * 16, 16);
					}
					int skinIndex = g.AddSkin(nodes.ToList(), ibm, nodes[0]);
					g.SceneRoots.Add(g.AddNode(new Dictionary<string, object> { ["name"] = $"{name}_skin", ["mesh"] = mesh, ["skin"] = skinIndex }));
					any = true;
				}
			}
			return any;
		}

		// A GI_CollisionData blob is one convex hull on the joint CollisionDataRelated[k] names, in that joint's
		// space: Header[0] vertices (vec4) from byte 0, Header[2] faces whose start bytes sit at Header[8] and
		// point into the face list at Header[9] (each face: vertex count, then that many vertex indices).
		// The Euler count V - E + F = 2 holds for Header[0..2] on every hull checked (prisms and boxes).
		static void AddHulls(GraphicsInfo gi, GraphicsInfo.Joint[] joints, Dictionary<uint, int> slot, Func<int, Matrix4x4> rest,
			Dictionary<string, Matrix4x4?[]> endLocals, List<(string Suffix, Prim Hull)> hulls)
		{
			var data = gi.CollisionData ?? Array.Empty<GraphicsInfo.GI_CollisionData>();
			bool rigidProp = gi.SkinID == 0 && gi.BlendSkinID == 0;
			for (int k = 0; k < data.Length; k++)
			{
				var h = data[k].Header;
				byte[] b = data[k].collisionDataBlob;
				var local = new Vector3[h[0]];
				for (int v = 0; v < local.Length; v++)
				{
					local[v] = Space.Mirror(new Vector3(BitConverter.ToSingle(b, v * 16), BitConverter.ToSingle(b, v * 16 + 4), BitConverter.ToSingle(b, v * 16 + 8)));
				}
				var tris = new List<uint>();
				for (int f = 0; f < h[2]; f++)
				{
					int at = h[9] + b[h[8] + f];
					for (int c = 2; c < b[at]; c++)
					{
						// Mirroring flips handedness: swap the fan's winding back.
						tris.Add(b[at + 1]);
						tris.Add(b[at + c + 1]);
						tris.Add(b[at + c]);
					}
				}
				int j = k < gi.CollisionDataRelated.Length && slot.TryGetValue(gi.CollisionDataRelated[k], out int s) ? s : -1;
				Matrix4x4 restWorld = j >= 0 ? rest(j) : Matrix4x4.Identity;
				hulls.Add(($"hull{k}", HullPrim(local, tris, restWorld)));
				if (j < 0 || !rigidProp)
				{
					continue;
				}
				foreach (var clip in endLocals)
				{
					// Compose the clip's end locals (the rest local where the clip leaves a joint alone).
					Matrix4x4 end = Matrix4x4.Identity;
					for (int i = j; ; )
					{
						Matrix4x4 m;
						if (clip.Value[i] is Matrix4x4 animated)
						{
							m = animated;
						}
						else
						{
							var (t, q) = JointLocal(joints[i]);
							m = Matrix4x4.CreateFromQuaternion(q) * Matrix4x4.CreateTranslation(t);
						}
						end *= m;
						int parent = -1;
						if (i == 0 || !slot.TryGetValue(joints[i].ParentJointIndex, out parent) || parent == i)
						{
							break;
						}
						i = parent;
					}
					if (local.Any(p => Vector3.Distance(Vector3.Transform(p, end), Vector3.Transform(p, restWorld)) > 1e-3f))
					{
						hulls.Add(($"hull{k}_{clip.Key}", HullPrim(local, tris, end)));
					}
				}
			}
		}

		static Prim HullPrim(Vector3[] local, List<uint> tris, Matrix4x4 world)
		{
			var prim = new Prim();
			foreach (var p in local)
			{
				var w = Vector3.Transform(p, world);
				prim.Pos.Add(w.X);
				prim.Pos.Add(w.Y);
				prim.Pos.Add(w.Z);
			}
			prim.Idx.AddRange(tris);
			return prim;
		}

		// Joint rest pose: Matrix[0] is the local translation, Matrix[2] the local rotation quaternion.
		static (Vector3, Quaternion) JointLocal(GraphicsInfo.Joint j)
		{
			var t = Space.Mirror(new Vector3(j.Matrix[0].X, j.Matrix[0].Y, j.Matrix[0].Z));
			var q = new Quaternion(j.Matrix[2].X, j.Matrix[2].Y, j.Matrix[2].Z, j.Matrix[2].W);
			q = q.LengthSquared() > 1e-8f ? Quaternion.Normalize(Space.Mirror(q)) : Quaternion.Identity;
			return (t, q);
		}

		static Prim Get(Dictionary<(uint mat, int layer), Prim> prims, uint material)
		{
			if (!prims.TryGetValue((material, 0), out var p))
			{
				prims[(material, 0)] = p = new Prim();
			}
			return p;
		}

		static void AppendSkinned(Prim p, List<(float X, float Y, float Z, float U, float V, int J1, int J2, int J3, float W1, float W2, float W3, bool Conn)> v, Dictionary<uint, int> slot)
		{
			if (v.Count < 3)
			{
				return;
			}
			uint baseVertex = (uint)p.VertexCount;
			foreach (var d in v)
			{
				var pos = Space.Mirror(new Vector3(d.X, d.Y, d.Z));
				p.Pos.Add(pos.X); p.Pos.Add(pos.Y); p.Pos.Add(pos.Z);
				p.Uv.Add(d.U); p.Uv.Add(1 - d.V);
				int[] j = { d.J1, d.J2, d.J3 };
				float[] w = { Math.Max(d.W1, 0), Math.Max(d.W2, 0), Math.Max(d.W3, 0) };
				float sum = 0;
				for (int k = 0; k < 3; k++)
				{
					if (!slot.TryGetValue((uint)j[k], out int s))
					{
						w[k] = 0;
						s = 0;
					}
					j[k] = s;
					sum += w[k];
				}
				if (sum <= 1e-6f)
				{
					w[0] = 1;
					sum = 1;
				}
				for (int k = 0; k < 3; k++)
				{
					p.Joints.Add((ushort)j[k]);
					p.Weights.Add(w[k] / sum);
				}
				p.Joints.Add(0);
				p.Weights.Add(0);
			}
			Meshes.Strip(p, baseVertex, v.Count, k => v[k].Conn);
		}

		// Object names can be authoring paths ("|Pier|act_CRASH", "\Bossarea\...\act_CORTEX_TRAINING_MINIBOSS");
		// keep the leaf, capped so object directories stay well inside MAX_PATH.
		static string Safe(string name)
		{
			string leaf = (name ?? "").TrimEnd('\0').Split('\\', '/', '|').LastOrDefault(s => s.Length > 0) ?? "";
			var sb = new StringBuilder();
			foreach (char c in leaf.Length > 48 ? leaf.Substring(0, 48) : leaf)
			{
				sb.Append(char.IsLetterOrDigit(c) || c == '_' || c == '-' ? c : '_');
			}
			return sb.Length > 0 ? sb.ToString() : "unnamed";
		}

		// ---- images (.psm) --------------------------------------------------------------------------

		// A .psm is a run of (texture id, material id, Texture, Material) records: a picture split into tiles.
		// Same walk as CrashModded's twinsdump "psm" command; the material is skipped by its known layout.
		static string Images(string path, string archiveName)
		{
			string rel = archiveName.Substring(0, archiveName.Length - 4);
			// The HUD and menu art: Startup/Icons.psm, Startup/Decal.ptc (one record, same layout) and the level
			// title badges. Their texels hold half-range colour (<= 0x7F): the game draws the sprites with a 2x
			// vertex colour, so ui/ stores them at the brightness they show at.
			bool titles = rel.StartsWith("Language/Titles/", StringComparison.OrdinalIgnoreCase);
			bool ui = titles || rel.StartsWith("Startup/", StringComparison.OrdinalIgnoreCase);
			string dir = titles ? Path.Combine(s_out, "ui", "titles", Path.GetFileName(Path.GetDirectoryName(rel)))
			           : ui ? Path.Combine(s_out, "ui", "icons") : Path.Combine(s_out, "images", Path.GetDirectoryName(rel));
			string stem = Path.GetFileName(rel);
			Directory.CreateDirectory(dir);
			var raw = File.ReadAllBytes(path);
			int saved = 0;
			using (var ms = new MemoryStream(raw))
			using (var r = new BinaryReader(ms))
			{
				while (ms.Position + 16 < raw.Length)
				{
					r.ReadUInt32();
					r.ReadUInt32();
					var tex = new Texture();
					try
					{
						tex.Load(r, 0);
					}
					catch (Exception)
					{
						break;
					}
					var rgba = Pixels.Decode(tex);
					if (rgba != null && ui)
					{
						Pixels.Brighten(rgba);
					}
					if (rgba != null)
					{
						Pixels.SavePng(Path.Combine(dir, $"{stem}_{saved:D2}.png"), tex.Width, tex.Height, rgba);
						saved++;
						s_images++;
					}
					try
					{
						SkipMaterial(r);
					}
					catch (EndOfStreamException)
					{
						break;
					}
				}
			}
			return $"{saved} tiles";
		}

		// The Material after each texture, skipped by its known layout (the library's Material.Load throws here).
		static void SkipMaterial(BinaryReader r)
		{
			r.ReadUInt64();
			r.ReadInt32();
			r.ReadBytes(r.ReadInt32());
			int shaders = r.ReadInt32();
			for (int i = 0; i < shaders; i++)
			{
				uint kind = r.ReadUInt32();
				int extra = kind == 23 ? 12 : kind == 26 ? 20 : (kind == 16 || kind == 17) ? 4 : 0;
				r.ReadBytes(extra + 24 + 6 + 4 + 48 + 8);
			}
		}

		// ---- menu strings (Language/Code/<Language>.txt) --------------------------------------------

		// The front end and pause menu's own text, one string per line (blank lines separate groups; '~' is
		// a line break, and \ ^ < > { } ¦ ¬ select the font's button glyphs). Copied verbatim to ui/text/.
		static string Strings(string path, string archiveName)
		{
			string dir = Path.Combine(s_out, "ui", "text");
			Directory.CreateDirectory(dir);
			File.Copy(path, Path.Combine(dir, Path.GetFileName(archiveName)), true);
			return "strings copied";
		}

		// ---- fonts (.psf) ---------------------------------------------------------------------------

		// A .psf is TwinsPSF: a page count, that many (texture id, material id, Texture, Material) pages, then
		// a glyph count, the first character code (32) and one vec4 per character: (u, v, width, height) in
		// 1/16 texels, v measured up from the page's bottom edge to the cell's top. The page is not a field:
		// u carries it in its low mantissa bits, (bits(u) - bits(floor(u))) - 1 (glyph '0' sits on page 1 at
		// u 8.000002, '4' on page 0 at 8.000001, '%' on page 2 at 8.000003; the space is exactly 8.0, no page).
		// Written as ui/fonts/<font>.font.json: { "pages": [...], "glyphs": { "<code>": [page, x, y, w, h] } }
		// in whole page pixels, y from the top (page -1 is a blank, advance-only glyph), plus every drawn
		// glyph cut out as ui/fonts/<font>/<code>.png - engine UI images show whole textures, not sub-rects.
		static string Font(string path, string archiveName)
		{
			string stem = Path.GetFileNameWithoutExtension(archiveName);
			string dir = Path.Combine(s_out, "ui", "fonts");
			Directory.CreateDirectory(dir);
			var pages = new List<object>();
			var glyphs = new Dictionary<string, object>();
			int first;
			var decoded = new List<(byte[] Rgba, int W, int H)>();
			Directory.CreateDirectory(Path.Combine(dir, stem));
			using (var r = new BinaryReader(File.OpenRead(path)))
			{
				int count = r.ReadInt32();
				for (int i = 0; i < count; i++)
				{
					r.ReadUInt32();
					r.ReadUInt32();
					var tex = new Texture();
					tex.Load(r, 0);
					SkipMaterial(r);
					var rgba = Pixels.Decode(tex) ?? throw new InvalidDataException($"font page {i}: undecodable {tex.PixelFormat}");
					Pixels.Brighten(rgba);
					string file = $"{stem}_{i:D2}.png";
					Pixels.SavePng(Path.Combine(dir, file), tex.Width, tex.Height, rgba);
					pages.Add(new Dictionary<string, object> { ["file"] = file, ["width"] = tex.Width, ["height"] = tex.Height });
					s_images++;
					decoded.Add((rgba, tex.Width, tex.Height));
				}
				int glyphCount = r.ReadInt32();
				first = r.ReadInt32();
				for (int i = 0; i < glyphCount; i++)
				{
					float u = r.ReadSingle(), v = r.ReadSingle(), w = r.ReadSingle(), h = r.ReadSingle();
					if (w <= 0 || h <= 0)
					{
						continue;
					}
					int page = BitConverter.ToInt32(BitConverter.GetBytes(u), 0) - BitConverter.ToInt32(BitConverter.GetBytes((float)Math.Floor(u)), 0) - 1;
					int gx = (int)(u / 16f), gw = (int)Math.Round(w / 16f), gh = (int)Math.Round(h / 16f);
					int gy = page >= 0 && page < decoded.Count ? (int)(decoded[page].H - v / 16f) : 0;
					glyphs[(first + i).ToString()] = new List<object> { page, gx, gy, gw, gh };
					if (page < 0 || page >= decoded.Count)
					{
						continue;
					}
					var src = decoded[page];
					var cut = new byte[gw * gh * 4];
					for (int y = 0; y < gh; y++)
					{
						for (int x = 0; x < gw; x++)
						{
							int sx = gx + x, sy = gy + y;
							if (sx >= 0 && sx < src.W && sy >= 0 && sy < src.H)
							{
								Buffer.BlockCopy(src.Rgba, (sy * src.W + sx) * 4, cut, (y * gw + x) * 4, 4);
							}
						}
					}
					Pixels.SavePng(Path.Combine(dir, stem, $"{first + i}.png"), gw, gh, cut);
				}
			}
			var sb = new StringBuilder();
			Json.Write(sb, new Dictionary<string, object> { ["pages"] = pages, ["glyphs"] = glyphs });
			File.WriteAllText(Path.Combine(dir, stem + ".font.json"), sb.ToString());
			return $"{pages.Count} pages, {glyphs.Count} glyphs";
		}
	}
}
