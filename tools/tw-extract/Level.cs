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
		// and these do not block the player: camera-only, rigid-body-only and AI-only, and the water
		// surface (12) - on the rig Crash wades through it down the seabed and drowns on the
		// drowning plane (23) beneath; exported solid, he walked on the sea.
		static readonly HashSet<int> s_ignored = new HashSet<int> { 12, 20, 25, 27 };

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
			bool[] pieceFlip = PieceFlips(col, verts, flip);

			string dir = Path.Combine(outRoot, "models", "collision", level);
			Directory.CreateDirectory(dir);
			foreach (var stale in Directory.GetFiles(dir, Path.GetFileName(level) + "_col_*"))
			{
				File.Delete(stale);
			}
			int written = 0, skipped = 0, turned = 0;
			foreach (var kind in new[] { "solid", "deadly" })
			{
				bool deadly = kind == "deadly";
				var tris = Enumerable.Range(0, col.Tris.Count).Where(i => !s_ignored.Contains(col.Tris[i].Surface) && s_deadly.Contains(col.Tris[i].Surface) == deadly).OrderBy(i =>
				{
					var t = col.Tris[i];
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
				foreach (int i in tris)
				{
					if (prim != null && remap.Count + 3 > kMaxPieceVertices)
					{
						Flush();
					}
					prim = prim ?? new Prim();
					var t = col.Tris[i];
					bool asStored = flip ^ pieceFlip[i];
					// Floor-angle triangles always face up, even inside a piece whose floors mostly
					// do: the rest are thin sheets and folds Crash stands on from above. A real
					// ceiling turned up still blocks from below (Jolt collides with back faces).
					Vector3 normal = asStored
						? Vector3.Cross(verts[t.Vert2] - verts[t.Vert1], verts[t.Vert3] - verts[t.Vert1])
						: Vector3.Cross(verts[t.Vert3] - verts[t.Vert1], verts[t.Vert2] - verts[t.Vert1]);
					if (normal.Y < -0.64f * normal.Length())
					{
						asStored = !asStored;
						turned++;
					}
					uint a = Vertex(t.Vert1), b = Vertex(t.Vert2), c = Vertex(t.Vert3);
					prim.Idx.AddRange(asStored ? new[] { a, b, c } : new[] { a, c, b });
				}
				Flush();
			}
			int flipped = pieceFlip.Count(f => f);
			return $"collision {col.Tris.Count} tris in {written} pieces ({col.Tris.Count(t => s_deadly.Contains(t.Surface))} deadly, {skipped} non-blocking skipped, winding {(flip ? "as stored" : "reversed")}, {flipped} tris in inside-out pieces turned, {turned} down-facing floors turned up)";
		}

		// The PS2 data winds whole connected pieces inside out (a cliff top or a rock cap whose floor
		// faces down). The game collides both sides, but Jolt builds active edges and ground contacts
		// from the winding, and Crash lost the ground standing on such a piece. Returns, per triangle,
		// whether its edge-connected piece must be turned so most of its floor area faces up.
		static bool[] PieceFlips(ColData col, Vector3[] verts, bool flip)
		{
			int n = col.Tris.Count;
			var canonical = new Dictionary<(int, int, int), int>();
			int Id(int v)
			{
				var p = verts[v];
				var key = ((int)Math.Round(p.X * 1000f), (int)Math.Round(p.Y * 1000f), (int)Math.Round(p.Z * 1000f));
				if (!canonical.TryGetValue(key, out int id))
				{
					id = canonical[key] = canonical.Count;
				}
				return id;
			}
			var parent = Enumerable.Range(0, n).ToArray();
			int Find(int x)
			{
				while (parent[x] != x)
				{
					parent[x] = parent[parent[x]];
					x = parent[x];
				}
				return x;
			}
			var byEdge = new Dictionary<long, int>();
			for (int i = 0; i < n; i++)
			{
				var t = col.Tris[i];
				int[] ids = { Id(t.Vert1), Id(t.Vert2), Id(t.Vert3) };
				for (int e = 0; e < 3; e++)
				{
					int u = ids[e], v = ids[(e + 1) % 3];
					long key = u < v ? ((long)u << 32) | (uint)v : ((long)v << 32) | (uint)u;
					if (byEdge.TryGetValue(key, out int j))
					{
						parent[Find(i)] = Find(j);
					}
					else
					{
						byEdge[key] = i;
					}
				}
			}
			// Floor-angle area facing up vs down per piece, in the winding the writer uses.
			var upArea = new float[n];
			var downArea = new float[n];
			for (int i = 0; i < n; i++)
			{
				var t = col.Tris[i];
				var cross = flip
					? Vector3.Cross(verts[t.Vert2] - verts[t.Vert1], verts[t.Vert3] - verts[t.Vert1])
					: Vector3.Cross(verts[t.Vert3] - verts[t.Vert1], verts[t.Vert2] - verts[t.Vert1]);
				float len = cross.Length();
				if (len <= 0f)
				{
					continue;
				}
				float ny = cross.Y / len;
				int root = Find(i);
				if (ny > 0.64f)
				{
					upArea[root] += len;
				}
				else if (ny < -0.64f)
				{
					downArea[root] += len;
				}
			}
			var result = new bool[n];
			for (int i = 0; i < n; i++)
			{
				int root = Find(i);
				result[i] = downArea[root] > upArea[root];
			}
			return result;
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
				// The layer's AI positions (item 3) and paths (item 4), which instances reference by index.
				var points = section.ContainsItem(3) && section.GetItem<TwinsItem>(3) is TwinsSection ps
					? ps.Records.OfType<Position>().ToDictionary(p => p.ID, p => Space.Mirror(new Vector3(p.Pos.X, p.Pos.Y, p.Pos.Z)))
					: new Dictionary<uint, Vector3>();
				var paths = section.ContainsItem(4) && section.GetItem<TwinsItem>(4) is TwinsSection pa
					? pa.Records.OfType<Twinsanity.Path>().ToDictionary(p => p.ID, p => p.Positions.Select(q => Space.Mirror(new Vector3(q.X, q.Y, q.Z))).ToList())
					: new Dictionary<uint, List<Vector3>>();
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
					// The actor subtype (script condition ActorSubtypeEquals), e.g. which one-shot
					// clip a wumpa tree plays or whether a falling log starts already down.
					if (ins.UnkI323.Count > 0 && ins.UnkI323[0] != 0)
					{
						entry["subtype"] = ins.UnkI323[0];
					}
					// Instance flags (e.g. 0x80000 starts a seagull already flying), the instances it
					// references (a creature spawner's template), its AI positions and its first path.
					entry["flags"] = ins.Flags;
					if (ins.InstanceIDs.Count > 0)
					{
						entry["links"] = ins.InstanceIDs.Select(i => (uint)i).ToArray();
					}
					var own = ins.PositionIDs.Where(i => points.ContainsKey(i)).Select(i => points[i]).ToList();
					if (own.Count > 0)
					{
						entry["points"] = own.Select(v => new[] { v.X, v.Y, v.Z }).ToArray();
					}
					if (ins.PathIDs.Count > 0 && paths.TryGetValue(ins.PathIDs[0], out var path))
					{
						entry["path"] = path.Select(v => new[] { v.X, v.Y, v.Z }).ToArray();
					}
					if (ins.UnkI323.Count > 2)
					{
						entry["params"] = ins.UnkI323.ToArray();
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
