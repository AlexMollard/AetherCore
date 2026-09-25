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
	// A chunk's playable data from its .rm2: collision as bakeable glTF pieces plus a levels/<chunk>.level.json
	// manifest (scenery paths, collision pieces, spawn, object instances) that a runtime assembles.
	static class LevelExport
	{
		// The engine refuses mesh colliders above kMaxColliderMeshVertices (ColliderMeshSource.hpp).
		const int kMaxPieceVertices = 4096;
		// Triangles are grouped by the XZ cell of their centroid before filling pieces, so a piece is a
		// compact patch of ground rather than a strip across the whole level.
		const float kCellSize = 16f;

		// A collision triangle's Surface is a SurfaceTypes value (DefaultEnums.cs); the CollisionSurface records
		// only carry each type's sounds and particles. These kill on contact:
		static readonly HashSet<int> s_deadly = new HashSet<int> { 3, 4, 5, 23, 26 };
		// and these do not block the player: camera-only, rigid-body-only and AI-only.
		static readonly HashSet<int> s_ignored = new HashSet<int> { 20, 25, 27 };

		public static string Write(TwinsFile file, string level, string outRoot, Dictionary<uint, string> models)
		{
			string stem = Path.GetFileName(level);
			var manifest = new Dictionary<string, object> { ["level"] = level };
			string sceneryDir = Path.Combine(outRoot, "models", "scenery", level);
			manifest["scenery"] = new[] { stem + ".gltf", stem + "_dynamic.gltf" }.Select(n => Program.VfsPath(Path.Combine(sceneryDir, n))).ToList();
			manifest["sky"] = Program.VfsPath(Path.Combine(sceneryDir, stem + "_sky.gltf"));

			string collisionNote = Collision(file, level, outRoot, manifest);
			string instanceNote = Instances(file, models, manifest);

			string path = Path.Combine(outRoot, "levels", level + ".level.json");
			Directory.CreateDirectory(Path.GetDirectoryName(path));
			var sb = new StringBuilder();
			Json.Write(sb, manifest);
			File.WriteAllText(path, sb.ToString());
			return collisionNote + ", " + instanceNote;
		}

		static string Collision(TwinsFile file, string level, string outRoot, Dictionary<string, object> manifest)
		{
			var pieces = new List<object>();
			manifest["collision"] = pieces;
			if (!file.ContainsItem(9) || !(file.GetItem<TwinsItem>(9) is ColData col) || col.isEmpty || col.Tris.Count == 0)
			{
				return "no collision";
			}

			var verts = col.Vertices.Select(p => Space.Mirror(new Vector3(p.X, p.Y, p.Z))).ToArray();
			// X is mirrored, which reverses handedness: (1, 3, 2) is the original triangle's front face. Which
			// side the game treats as front is settled by the floors: whichever order makes more up-facing
			// floor triangles is the one written.
			int up = 0, down = 0;
			foreach (var t in col.Tris)
			{
				float ny = Vector3.Normalize(Vector3.Cross(verts[t.Vert3] - verts[t.Vert1], verts[t.Vert2] - verts[t.Vert1])).Y;
				up += ny > 0.7f ? 1 : 0;
				down += ny < -0.7f ? 1 : 0;
			}
			bool flip = down > up;

			string dir = Path.Combine(outRoot, "models", "collision", level);
			Directory.CreateDirectory(dir);
			foreach (var stale in Directory.GetFiles(dir, Path.GetFileName(level) + "_col_*"))
			{
				File.Delete(stale);
			}
			int written = 0, skipped = 0;
			foreach (var kind in new[] { "solid", "deadly" })
			{
				bool deadly = kind == "deadly";
				var tris = col.Tris.Where(t => !s_ignored.Contains(t.Surface) && s_deadly.Contains(t.Surface) == deadly).OrderBy(t =>
				{
					var c = (verts[t.Vert1] + verts[t.Vert2] + verts[t.Vert3]) / 3f;
					return ((long)Math.Floor(c.X / kCellSize) << 32) ^ (long)Math.Floor(c.Z / kCellSize) & 0xFFFFFFFF;
				}).ToList();
				skipped += deadly ? 0 : col.Tris.Count(t => s_ignored.Contains(t.Surface));

				Prim prim = null;
				var remap = new Dictionary<int, uint>();
				void Flush()
				{
					if (prim == null || prim.Idx.Count == 0)
					{
						return;
					}
					var g = new Gltf();
					int mesh = g.AddMesh("collision", new[] { (prim, -1) });
					g.SceneRoots.Add(g.AddNode(new Dictionary<string, object> { ["name"] = "collision", ["mesh"] = mesh }));
					string name = $"{Path.GetFileName(level)}_col_{kind}_{written:D3}.gltf";
					g.Save(Path.Combine(dir, name));
					pieces.Add(new Dictionary<string, object> { ["path"] = Program.VfsPath(Path.Combine(dir, name)), ["deadly"] = deadly });
					written++;
					prim = null;
					remap.Clear();
				}
				uint Vertex(int v)
				{
					if (!remap.TryGetValue(v, out uint index))
					{
						index = remap[v] = (uint)prim.VertexCount;
						prim.Pos.Add(verts[v].X);
						prim.Pos.Add(verts[v].Y);
						prim.Pos.Add(verts[v].Z);
					}
					return index;
				}
				foreach (var t in tris)
				{
					if (prim != null && remap.Count + 3 > kMaxPieceVertices)
					{
						Flush();
					}
					prim = prim ?? new Prim();
					uint a = Vertex(t.Vert1), b = Vertex(t.Vert2), c = Vertex(t.Vert3);
					prim.Idx.AddRange(flip ? new[] { a, b, c } : new[] { a, c, b });
				}
				Flush();
			}
			return $"collision {col.Tris.Count} tris in {written} pieces ({col.Tris.Count(t => s_deadly.Contains(t.Surface))} deadly, {skipped} non-blocking skipped, winding {(flip ? "as stored" : "reversed")})";
		}

		static string Instances(TwinsFile file, Dictionary<uint, string> models, Dictionary<string, object> manifest)
		{
			var names = new Dictionary<uint, string>();
			if (file.ContainsItem(10) && file.GetItem<TwinsItem>(10) is TwinsSection code && code.ContainsItem(0)
			    && code.GetItem<TwinsItem>(0) is TwinsSection objects)
			{
				foreach (var o in objects.Records.OfType<GameObject>())
				{
					names[o.ID] = o.Name;
				}
			}

			var list = new List<object>();
			for (uint layer = 0; layer < 8; layer++)
			{
				if (!file.ContainsItem(layer) || !(file.GetItem<TwinsItem>(layer) is TwinsSection section) || !section.ContainsItem(6)
				    || !(section.GetItem<TwinsItem>(6) is TwinsSection placed))
				{
					continue;
				}
				foreach (var ins in placed.Records.OfType<Instance>())
				{
					var pos = Space.Mirror(new Vector3(ins.Pos.X, ins.Pos.Y, ins.Pos.Z));
					var entry = new Dictionary<string, object>
					{
						["id"] = ins.ID,
						["layer"] = layer,
						["object"] = (uint)ins.ObjectID,
						["name"] = names.TryGetValue(ins.ObjectID, out string n) ? Leaf(n) : "",
						["position"] = new[] { pos.X, pos.Y, pos.Z },
						["euler"] = Euler(ins),
					};
					if (models.TryGetValue(ins.ObjectID, out string model))
					{
						entry["model"] = model;
					}
					list.Add(entry);
					if (ins.ObjectID == 0 && !manifest.ContainsKey("spawn"))
					{
						manifest["spawn"] = entry;
					}
				}
			}
			manifest["instances"] = list;
			return $"{list.Count} instances{(manifest.ContainsKey("spawn") ? " (spawn found)" : "")}";
		}

		static string Leaf(string name) => name.Split('\\', '/', '|').Last(s => s.Length > 0);

		// The Twinsanity editor places an instance at p * RotX(x) * RotY(-y) * RotZ(-z) in its (already
		// X-mirrored) world, row vectors, angles a ushort fraction of a turn. Returned as the engine's Euler
		// degrees (TransformUtils.hpp ComposeTransform: R = Ry * Rx * Rz, column vectors).
		static float[] Euler(Instance ins)
		{
			float Turn(ushort v) => v / 65536f * 2f * (float)Math.PI;
			var m = Matrix4x4.CreateRotationX(Turn(ins.RotX)) * Matrix4x4.CreateRotationY(-Turn(ins.RotY)) * Matrix4x4.CreateRotationZ(-Turn(ins.RotZ));
			// Column-vector R = transpose(m), so R[r][c] = m.M(c+1)(r+1).
			float x = (float)Math.Asin(Math.Max(-1f, Math.Min(1f, -m.M32)));
			float y = (float)Math.Atan2(m.M31, m.M33);
			float z = (float)Math.Atan2(m.M12, m.M22);
			const float deg = 180f / (float)Math.PI;
			return new[] { x * deg, y * deg, z * deg };
		}
	}
}
