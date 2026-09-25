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

		public static string Write(TwinsFile file, string sm2Path, string level, string outRoot, Dictionary<uint, string> models)
		{
			string stem = Path.GetFileName(level);
			var manifest = new Dictionary<string, object> { ["level"] = level };
			string sceneryDir = Path.Combine(outRoot, "models", "scenery", level);
			manifest["scenery"] = new[] { stem + ".gltf", stem + "_dynamic.gltf" }.Select(n => Program.VfsPath(Path.Combine(sceneryDir, n))).ToList();
			manifest["sky"] = Program.VfsPath(Path.Combine(sceneryDir, stem + "_sky.gltf"));

			string collisionNote = Collision(file, level, outRoot, manifest);
			string instanceNote = Instances(file, models, manifest);
			Links(sm2Path, level, outRoot, manifest);

			string path = Path.Combine(outRoot, "levels", level + ".level.json");
			Directory.CreateDirectory(Path.GetDirectoryName(path));
			var sb = new StringBuilder();
			Json.Write(sb, manifest);
			File.WriteAllText(path, sb.ToString());
			return collisionNote + ", " + instanceNote;
		}

		// Chunk links (SM2 section 5) position the neighbouring chunks. ChunkMatrix is the row-vector
		// matrix placing the linked chunk relative to this one: its last row is the translation, the
		// 3x3 above it the rotation. All exported content is X-mirrored, so the transform conjugates
		// with S = diag(-1,1,1): negate row3.X and the four off-diagonal X entries of the rotation
		// block (S*M*S for the rotation). Verified against the collision seams: for beach the
		// mirrored ChunkMatrix gives huba (+70.40002, 0, +41.6001) and hubc (-182.0632, 0, -41.0094),
		// aligning 635 and 497 shared boundary collision vertices with max mismatch 0.0003 and 0.0;
		// the mirrored ObjectMatrix row3 points the opposite (wrong) way. Every Hub link so far
		// carries an identity rotation to float precision.
		// Flags' low byte is the link kind: 1 a seamless neighbour (huba, hubc, bossarea, alwayson),
		// 2 a door into another space (totemex, docent, the level entrances), 0 the boat trip. Only
		// kind-1 links are consistent around the hub's loops, so kind-2/0 are not spatial neighbours.
		static void Links(string sm2Path, string level, string outRoot, Dictionary<string, object> manifest)
		{
			var links = new List<object>();
			manifest["links"] = links;
			// ChunkLinks live in the .sm2 (section 5), not the .rm2 the rest of the manifest reads.
			if (!File.Exists(sm2Path))
			{
				return;
			}
			var sm2 = new TwinsFile();
			sm2.LoadFile(sm2Path, TwinsFile.FileType.SM2);
			if (!(sm2.GetItem<TwinsItem>(5) is ChunkLinks chunkLinks))
			{
				return;
			}
			foreach (var link in chunkLinks.Links)
			{
				if (link.ChunkMatrix == null || link.ChunkMatrix.Length < 4 || string.IsNullOrEmpty(link.Path))
				{
					continue;
				}
				Pos[] m = link.ChunkMatrix;
				float[] rot =
				{
					m[0].X, -m[0].Y, -m[0].Z,
					-m[1].X, m[1].Y, m[1].Z,
					-m[2].X, m[2].Y, m[2].Z,
				};
				float maxOff = 0f;
				for (int i = 0; i < 9; i++)
				{
					maxOff = Math.Max(maxOff, Math.Abs(rot[i] - (i % 4 == 0 ? 1f : 0f)));
				}
				var entry = new Dictionary<string, object>
				{
					["chunk"] = LinkChunk(level, link.Path, outRoot),
					["offset"] = new[] { -m[3].X, m[3].Y, m[3].Z },
					["flags"] = link.Flags,
				};
				if (maxOff > 1e-4f)
				{
					entry["rotation"] = Quat(rot);
				}
				links.Add(entry);
			}
		}

		// Link paths are lowercase ("levels\earth\hub\huba") while the extracted tree keeps the
		// archive's casing ("Earth/Hub"); resolve the segments against the files already on disk and
		// fall back to the current level's directory + leaf for a target not written yet.
		static string LinkChunk(string level, string linkPath, string outRoot)
		{
			var segs = linkPath.Split('\\', '/').Where(s => s.Length > 0 && !s.Equals("levels", StringComparison.OrdinalIgnoreCase)).ToList();
			string dir = Path.Combine(outRoot, "levels");
			for (int i = 0; dir != null && i < segs.Count - 1; i++)
			{
				dir = Directory.GetFileSystemEntries(dir).FirstOrDefault(e => string.Equals(Path.GetFileName(e), segs[i], StringComparison.OrdinalIgnoreCase));
			}
			if (dir != null)
			{
				string file = Directory.GetFiles(dir).FirstOrDefault(f => string.Equals(Path.GetFileName(f), segs.Last() + ".level.json", StringComparison.OrdinalIgnoreCase));
				if (file != null)
				{
					return Program.VfsPath(file);
				}
			}
			return Program.VfsPath(Path.Combine(Path.Combine(outRoot, "levels", level), segs.Last() + ".level.json"));
		}

		// Quaternion (x, y, z, w) from a row-major 3x3 rotation matrix (Shepperd).
		static float[] Quat(float[] r)
		{
			float tr = r[0] + r[4] + r[8];
			double x, y, z, w;
			if (tr > 0)
			{
				double s = Math.Sqrt(tr + 1.0) * 2;
				w = 0.25 * s;
				x = (r[7] - r[5]) / s;
				y = (r[2] - r[6]) / s;
				z = (r[3] - r[1]) / s;
			}
			else if (r[0] > r[4] && r[0] > r[8])
			{
				double s = Math.Sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
				w = (r[7] - r[5]) / s;
				x = 0.25 * s;
				y = (r[1] + r[3]) / s;
				z = (r[2] + r[6]) / s;
			}
			else if (r[4] > r[8])
			{
				double s = Math.Sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
				w = (r[2] - r[6]) / s;
				x = (r[1] + r[3]) / s;
				y = 0.25 * s;
				z = (r[5] + r[7]) / s;
			}
			else
			{
				double s = Math.Sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
				w = (r[3] - r[1]) / s;
				x = (r[2] + r[6]) / s;
				y = (r[5] + r[7]) / s;
				z = 0.25 * s;
			}
			return new[] { (float)x, (float)y, (float)z, (float)w };
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
					// The object's tuning, e.g. a character's speeds, gravities and jump heights
					// (DefaultEnums.CharacterInstanceFloats).
					if (ins.UnkI322.Count > 1)
					{
						entry["floats"] = ins.UnkI322.ToArray();
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
