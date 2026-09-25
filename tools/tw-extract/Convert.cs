using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Twinsanity;
using Path = System.IO.Path;

namespace TwExtract
{
	// Everything one level file's graphics section holds, keyed by record ID per item type.
	sealed class Gfx
	{
		public readonly Dictionary<uint, Texture> Textures = new Dictionary<uint, Texture>();
		public readonly Dictionary<uint, Twinsanity.Material> Materials = new Dictionary<uint, Twinsanity.Material>();
		public readonly Dictionary<uint, Model> Models = new Dictionary<uint, Model>();
		public readonly Dictionary<uint, RigidModel> Rigids = new Dictionary<uint, RigidModel>();
		public readonly Dictionary<uint, LodModel> Lods = new Dictionary<uint, LodModel>();
		public readonly Dictionary<uint, Skydome> Skydomes = new Dictionary<uint, Skydome>();
		public readonly Dictionary<uint, Skin> Skins = new Dictionary<uint, Skin>();
		public readonly Dictionary<uint, BlendSkin> BlendSkins = new Dictionary<uint, BlendSkin>();

		// The graphics section is 11 in an .rm2 and 6 in an .sm2. Sub-section IDs differ between the two
		// (RigidModel is 3 in one, 6 in the other), so items are grouped by type instead of by sub-ID.
		public Gfx(TwinsFile file, uint sectionId)
		{
			if (!file.ContainsItem(sectionId))
			{
				return;
			}
			foreach (var item in Items(file.GetItem<TwinsSection>(sectionId)))
			{
				switch (item)
				{
					case Texture t: Textures[t.ID] = t; break;
					case Twinsanity.Material m: Materials[m.ID] = m; break;
					case Model m: Models[m.ID] = m; break;
					case RigidModel r: Rigids[r.ID] = r; break;
					case LodModel l: Lods[l.ID] = l; break;
					case Skydome s: Skydomes[s.ID] = s; break;
					case Skin s: Skins[s.ID] = s; break;
					case BlendSkin b: BlendSkins[b.ID] = b; break;
				}
			}
		}

		public static IEnumerable<TwinsItem> Items(TwinsSection section)
		{
			foreach (var r in section.Records)
			{
				if (r is TwinsSection child)
				{
					foreach (var nested in Items(child))
					{
						yield return nested;
					}
				}
				else
				{
					yield return r;
				}
			}
		}
	}

	// Textures are written once, content-addressed, into <out>/textures/ and shared by every model.
	sealed class TextureStore
	{
		readonly string m_dir;
		readonly Dictionary<Texture, string> m_names = new Dictionary<Texture, string>();
		public int Written, Undecodable;

		public TextureStore(string outRoot)
		{
			m_dir = Path.Combine(outRoot, "textures");
			Directory.CreateDirectory(m_dir);
		}

		// File name (no directory) of the PNG for this texture, or null when its pixel format has no decoder.
		public string Name(Texture t)
		{
			if (m_names.TryGetValue(t, out var cached))
			{
				return cached;
			}
			var rgba = Pixels.Decode(t);
			string name = null;
			if (rgba == null)
			{
				Undecodable++;
			}
			else
			{
				name = Hash.Of(BitConverter.GetBytes(t.Width), BitConverter.GetBytes(t.Height), rgba) + ".png";
				string path = Path.Combine(m_dir, name);
				if (!File.Exists(path))
				{
					Pixels.SavePng(path, t.Width, t.Height, rgba);
					Written++;
				}
			}
			return m_names[t] = name;
		}
	}

	static class Pixels
	{
		static readonly BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;
		static T Field<T>(Texture t, string name) => (T)typeof(Texture).GetField(name, Private).GetValue(t);

		// RGBA8, straight alpha, rows top-down in the library's orientation. Decoded here rather than read from
		// Texture.RawData because the library stores GS alpha as (byte)(a << 1): 0x80 (opaque) wraps to 0, which
		// would turn every opaque texel transparent. PS2 alpha is 0..0x80, so this uses min(a * 2, 255).
		public static byte[] Decode(Texture t)
		{
			int w = t.Width, h = t.Height;
			var raw = Field<byte[]>(t, "imageData");
			if (raw == null)
			{
				return null;
			}
			var rgba = new byte[w * h * 4];
			switch (t.PixelFormat)
			{
				case Texture.TexturePixelFormat.PSMCT32:
					for (int i = 0; i < w * h; i++)
					{
						rgba[i * 4 + 0] = raw[i * 4 + 0];
						rgba[i * 4 + 1] = raw[i * 4 + 1];
						rgba[i * 4 + 2] = raw[i * 4 + 2];
						rgba[i * 4 + 3] = Alpha(raw[i * 4 + 3]);
					}
					return rgba;
				case Texture.TexturePixelFormat.PSMT8:
				{
					// Same GS-memory round trip as Texture.Load's PSMT8 branch, keeping the palette's raw alpha.
					var ez = new EzSwizzle();
					ez.writeTexPSMCT32(0, 1, 0, 0, Field<int>(t, "rrw"), Field<int>(t, "rrh"), raw);
					var index = new byte[w * h];
					ez.readTexPSMT8(0, Field<int>(t, "textureBufferWidth"), 0, 0, w, h, ref index);
					var pal = new byte[256 * 4];
					ez.readTexPSMCT32(Field<int>(t, "clutBufferBasePointer"), 1, 0, 0, 16, 16, ref pal);
					// CSM1 CLUT layout: entries 8..15 and 16..23 of every 32 are stored swapped.
					for (int block = 0; block < 8; block++)
					{
						for (int j = 8 + block * 32; j < 16 + block * 32; j++)
						{
							for (int b = 0; b < 4; b++)
							{
								(pal[j * 4 + b], pal[(j + 8) * 4 + b]) = (pal[(j + 8) * 4 + b], pal[j * 4 + b]);
							}
						}
					}
					for (int y = 0; y < h; y++)
					{
						int src = (h - 1 - y) * w; // Texture.Load flips PSMT8 vertically
						for (int x = 0; x < w; x++)
						{
							int p = index[src + x] * 4, o = (y * w + x) * 4;
							rgba[o + 0] = pal[p + 0];
							rgba[o + 1] = pal[p + 1];
							rgba[o + 2] = pal[p + 2];
							rgba[o + 3] = Alpha(pal[p + 3]);
						}
					}
					return rgba;
				}
				default:
					return null;
			}
		}

		static byte Alpha(byte gs) => (byte)Math.Min(gs * 2, 255);

		// Sprite art is drawn with a 2x vertex colour (MODULATE: tex * 0xFF >> 7), so bake that in.
		public static void Brighten(byte[] rgba)
		{
			for (int i = 0; i < rgba.Length; i++)
			{
				if ((i & 3) != 3)
				{
					rgba[i] = (byte)Math.Min(rgba[i] * 2, 255);
				}
			}
		}

		public static void SavePng(string path, int w, int h, byte[] rgba)
		{
			using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
			{
				var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
				var bgra = new byte[w * h * 4];
				for (int i = 0; i < w * h; i++)
				{
					bgra[i * 4 + 0] = rgba[i * 4 + 2];
					bgra[i * 4 + 1] = rgba[i * 4 + 1];
					bgra[i * 4 + 2] = rgba[i * 4 + 0];
					bgra[i * 4 + 3] = rgba[i * 4 + 3];
				}
				for (int y = 0; y < h; y++)
				{
					Marshal.Copy(bgra, y * w * 4, data.Scan0 + y * data.Stride, w * 4);
				}
				bmp.UnlockBits(data);
				bmp.Save(path, ImageFormat.Png);
			}
		}
	}

	static class Hash
	{
		public static string Of(params byte[][] parts)
		{
			using (var sha = SHA1.Create())
			{
				foreach (var p in parts)
				{
					sha.TransformBlock(p, 0, p.Length, null, 0);
				}
				sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
				return BitConverter.ToString(sha.Hash, 0, 8).Replace("-", "").ToLowerInvariant();
			}
		}
	}

	// A glTF under construction. Game materials become plain glTF materials (texture, alpha mode, double
	// sided), which the engine's bake turns into .material files beside the model.
	sealed class Export
	{
		public readonly Gltf Gltf = new Gltf();
		readonly Gfx m_gfx;
		readonly TextureStore m_textures;
		readonly Dictionary<(uint, bool, int), int> m_materialIndex = new Dictionary<(uint, bool, int), int>();
		readonly Dictionary<string, int> m_byName = new Dictionary<string, int>();
		readonly string m_texturesRel;
		// Object models (Program.Objects): lit by the level's light records, not prelit.
		readonly bool m_isObject;

		public Export(Gfx gfx, TextureStore textures, string texturesRelativeToGltf, bool isObject = false)
		{
			m_gfx = gfx;
			m_textures = textures;
			m_texturesRel = texturesRelativeToGltf;
			m_isObject = isObject;
		}

	// vertexLit: the primitive carries PS2 vertex colour, so the material takes Space.VertexGain.
	// layer: which DISTINCT texture-mapped shader record of the material to export. Sea and sky
	// materials carry two records: a base texture plus a blended overlay (the sea surface, the
	// cloud sheet), and the PS2 draws each over the same geometry. Exporting only the first used
	// to drop the sea/cloud layer entirely - the second record's texture appeared in no glTF.
	public int Material(uint materialId, bool vertexLit, int layer = 0)
	{
		if (m_materialIndex.TryGetValue((materialId, vertexLit, layer), out int index))
		{
			return index;
		}
		string texture = null;
		bool blend = false, mask = false;
		float cutoff = 0.5f;
		float scrollU = 0f, scrollV = 0f;
		if (m_gfx.Materials.TryGetValue(materialId, out var mat))
		{
			// Distinct texture-mapped records in order; identical repeats (per-context copies)
			// collapse into one layer. Sea and sky materials carry two: a base texture plus a
			// blended overlay (the sea surface, the cloud sheet) the PS2 draws over the same
			// geometry - exporting only the first dropped the sea/cloud layer entirely.
			var mapped = mat.Shaders
				.Where(s => s.TxtMapping == TwinsShader.TextureMapping.ON && s.TextureId != 0)
				.GroupBy(s => (s.TextureId, s.ShaderType, s.FloatParam[0], s.FloatParam[1], s.FloatParam[2], s.FloatParam[3]))
				.Select(g => g.First())
				.ToList();
			var shader = mapped.ElementAtOrDefault(layer)
			             // Layer 0 falls back to the editor-viewer pick so texture-less materials
			             // still export as before; deeper layers with no further record export nothing.
			             ?? (layer == 0 ? mat.Shaders.FirstOrDefault() : null);
			if (shader == null)
			{
				return -1;
			}
			if (shader.TxtMapping == TwinsShader.TextureMapping.ON && m_gfx.Textures.TryGetValue(shader.TextureId, out var tex))
			{
				texture = m_textures.Name(tex);
			}
			blend = shader.ABlending == TwinsShader.AlphaBlending.ON;
			mask = !blend && shader.ATest == Twinsanity.TwinsShader.AlphaTest.ON;
			cutoff = Math.Min(shader.AlphaValueToBeComparedTo * 2, 255) / 255f;
			// Shader types 23 and 26 carry float params, but they are not a UV scroll: every
			// material using them in the Hub is foliage (grass, leaves, ivy, palm fronds), and
			// scrolling them makes the leaves visibly slide. Only rig-measured rates scroll.
			if (texture != null && s_measuredScroll.TryGetValue(texture, out var measured))
			{
				scrollU = measured.U;
				scrollV = measured.V;
			}
		}
		// Named by content, one glTF material per name. The bake writes materials/<name>.material beside
		// the model, so equal names must mean equal content.
		string name = "m_" + Hash.Of(Encoding.UTF8.GetBytes($"{texture}|{blend}|{mask}|{cutoff:R}|{vertexLit}|{m_isObject}|{scrollU:R}|{scrollV:R}"));
		if (m_byName.TryGetValue(name, out index))
		{
			return m_materialIndex[(materialId, vertexLit, layer)] = index;
		}
		// doubleSided: the GS never culls, and the game leaves culling off for foliage, cloth and decals.
		// baseColorFactor above 1 is outside glTF's schema but not its maths: it carries the part of the
		// PS2's vertex-colour range COLOR_0 cannot (see Space.Channel), and the engine applies it as is.
		float gain = vertexLit ? Space.VertexGain : 1f;
		var gltfMat = new Dictionary<string, object>
		{
			["name"] = name,
			["pbrMetallicRoughness"] = new Dictionary<string, object>
			{
				["baseColorFactor"] = new[] { gain, gain, gain, 1f },
				["metallicFactor"] = 0f,
				["roughnessFactor"] = 1f,
			},
			["doubleSided"] = true,
		};
		if (texture != null)
		{
			((Dictionary<string, object>)gltfMat["pbrMetallicRoughness"])["baseColorTexture"] =
			        new Dictionary<string, object> { ["index"] = Gltf.TextureFor(m_texturesRel + "/" + texture) };
		}
		if (blend)
		{
			gltfMat["alphaMode"] = "BLEND";
		}
		else if (mask)
		{
			gltfMat["alphaMode"] = "MASK";
			gltfMat["alphaCutoff"] = cutoff;
		}
		if (blend || mask || vertexLit || m_isObject || scrollU != 0f || scrollV != 0f)
		{
			// Read back by AssetPacker's MeshProcessor (cgltf extras) into the baked
			// material's uvScroll.
			var extras = new Dictionary<string, object>();
			if (scrollU != 0f || scrollV != 0f)
			{
				extras["uv_scroll"] = new[] { scrollU, scrollV };
			}
			// Alpha-masked scenery is PS2 foliage/cutout card art (leaves, grass tufts,
			// fences): the renderer keeps it out of the shadow maps, like the original.
			if (mask)
			{
				extras["foliage"] = true;
			}
			// PS2 vertex colour is prelit on scenery: it carries the level's whole lighting, so
			// the renderer shows it as is instead of lighting it again (only real-time shadows
			// pull it down to the scene's ambient). Object models instead carry a constant
			// vertex colour and are lit at run time by the level's own light records
			// (beach.sm2 SceneryData), as the GS lit everything that was not scenery -
			// including their unvertex-coloured parts (Crash's body, the crab's shells), whose
			// COLOR_0 the GS would have read as 1.0.
			if (vertexLit || m_isObject)
			{
				extras[m_isObject ? "object_lit" : "baked_lighting"] = true;
			}
			if (extras.Count > 0)
			{
				gltfMat["extras"] = extras;
			}
		}
		return m_materialIndex[(materialId, vertexLit, layer)] = m_byName[name] = Gltf.AddMaterial(gltfMat);
	}

	// MEASURED FROM ORIGINAL, not disc data: these overlay textures animate in the game but
	// their shader records carry no speed (types 12/22 read FloatParam=[0,0,0,0] - the motion
	// is code-driven on the PS2), so the value comes from PCSX2 captures.
	// The sea scroll is V-only. The sea strips' U zigzags (0 -> 1 -> 0 mirrored across each
	// strip, the foam columns of the texture on the u~0 shoreline edge), so any U scroll sweeps
	// the foam band off the sand and back once per repeat - the "foam sits away from the
	// shore" bug of the old (0.055, 0). V runs along each strip, so a V scroll keeps the foam
	// on the waterline and slides its uneven width along the shore, which is what the rig
	// shows: at the pier-side shore (game 12,-70) the foam's land edge never moves while its
	// water edge breathes with a 5.1-5.3 s period, travelling toward the inland camera. The
	// texture's foam width has one dominant bulge per V repeat, so speed = 1 / period.
	// Evidence: logs/wateredge (rigd/ dense captures, sheet.png).
	private static readonly Dictionary<string, (float U, float V)> s_measuredScroll = new Dictionary<string, (float, float)>
	{
		["2b1f0286252911e6.png"] = (0f, -0.19f), // Earth-Hub beach sea blend layers
	};

	// How many DISTINCT texture-mapped layers a material exports (>= 1; texture-less
	// materials export exactly the layer-0 fallback).
	public int LayerCount(uint materialId)
	{
		if (!m_gfx.Materials.TryGetValue(materialId, out var mat))
		{
			return 1;
		}
		return Math.Max(1, mat.Shaders
			.Where(s => s.TxtMapping == TwinsShader.TextureMapping.ON && s.TextureId != 0)
			.GroupBy(s => (s.TextureId, s.ShaderType, s.FloatParam[0], s.FloatParam[1], s.FloatParam[2], s.FloatParam[3]))
			.Count());
	}

		public string Save(string gltfPath)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(gltfPath));
			Gltf.Save(gltfPath);
			// The editor reuses an existing <stem>.mesh without comparing it to the source, so a re-extraction
			// has to drop it to get the new glTF baked.
			File.Delete(Path.ChangeExtension(gltfPath, ".mesh"));
			return gltfPath;
		}
	}

	// Game space -> glTF space. Twinsanity is mirrored relative to glTF's right-handed frame; like the
	// Twinsanity editor, X is negated. Mirroring flips triangle winding, which Strip() accounts for.
	static class Space
	{
		public static Vector3 Mirror(Vector3 v) => new Vector3(-v.X, v.Y, v.Z);
		public static Quaternion Mirror(Quaternion q) => new Quaternion(q.X, -q.Y, -q.Z, q.W);

		// PS2 vertex colour byte: the GS treats 0x80 as 1.0 and multiplies in gamma space
		// (MODULATE is tex * col >> 7), and scenery sits well above 0x80 (around 0xB0). The engine
		// multiplies in linear space, so the equivalent factor is (byte / 128)^2.2. COLOR_0 must stay in
		// 0..1, so it holds (byte / 255)^2.2 and the material's base colour factor the rest.
		public static float Channel(int gs) => (float)Math.Pow(Math.Min(gs, 255) / 255.0, 2.2);
		public static readonly float VertexGain = (float)Math.Pow(255.0 / 128.0, 2.2);
	}

	static class Meshes
	{
		// Triangle-strip -> list, the rule the game data follows: vertex j+2's connection flag says whether
		// (j, j+1, j+2) is a triangle, alternating orientation. Emitted reversed because of the X mirror.
		public static void Strip(Prim p, uint baseVertex, int count, Func<int, bool> conn)
		{
			for (int j = 0; j + 2 < count; j++)
			{
				if (!conn(j + 2))
				{
					continue;
				}
				uint a = baseVertex + (uint)j, b = a + 1, c = a + 2;
				if (j % 2 == 0)
				{
					p.Idx.Add(b); p.Idx.Add(a); p.Idx.Add(c);
				}
				else
				{
					p.Idx.Add(a); p.Idx.Add(b); p.Idx.Add(c);
				}
			}
		}

		// Rigid mesh vertices through an optional game-space affine transform (row-vector: p' = p * m).
		public static void AppendModel(Prim p, Model.SubModel sub, Matrix4x4 xf)
		{
			var v = sub.Vertexes;
			if (v == null || v.Count < 3)
			{
				return;
			}
			uint baseVertex = (uint)p.VertexCount;
			foreach (var d in v)
			{
				var pos = Space.Mirror(Vector3.Transform(new Vector3(d.X, d.Y, d.Z), xf));
				var n = Vector3.TransformNormal(new Vector3(d.NX, d.NY, d.NZ), xf);
				n = n.LengthSquared() > 1e-12f ? Vector3.Normalize(Space.Mirror(n)) : Vector3.Zero; // filled by FillNormals
				p.Pos.Add(pos.X); p.Pos.Add(pos.Y); p.Pos.Add(pos.Z);
				p.Nrm.Add(n.X); p.Nrm.Add(n.Y); p.Nrm.Add(n.Z);
				p.Uv.Add(d.U); p.Uv.Add(d.V);
				// The emit colour is additive light baked per vertex; the editor sums it the same way.
				p.Col.Add(Space.Channel(d.R + d.ER)); p.Col.Add(Space.Channel(d.G + d.EG)); p.Col.Add(Space.Channel(d.B + d.EB));
				p.Col.Add(1f);
			}
			Strip(p, baseVertex, v.Count, j => v[j].Conn);
		}

		// Vertices without authored normals (skins, some props) get area-weighted face normals.
		public static void FillNormals(Prim p)
		{
			var acc = new Vector3[p.VertexCount];
			for (int i = 0; i + 2 < p.Idx.Count; i += 3)
			{
				int a = (int)p.Idx[i], b = (int)p.Idx[i + 1], c = (int)p.Idx[i + 2];
				var n = Vector3.Cross(At(p.Pos, b) - At(p.Pos, a), At(p.Pos, c) - At(p.Pos, a));
				acc[a] += n; acc[b] += n; acc[c] += n;
			}
			bool hadNormals = p.Nrm.Count == p.Pos.Count;
			for (int v = 0; v < p.VertexCount; v++)
			{
				if (hadNormals && At(p.Nrm, v).LengthSquared() > 0.5f)
				{
					continue;
				}
				var n = acc[v].LengthSquared() > 1e-20f ? Vector3.Normalize(acc[v]) : Vector3.UnitY;
				if (!hadNormals)
				{
					p.Nrm.Add(n.X); p.Nrm.Add(n.Y); p.Nrm.Add(n.Z);
				}
				else
				{
					p.Nrm[v * 3] = n.X; p.Nrm[v * 3 + 1] = n.Y; p.Nrm[v * 3 + 2] = n.Z;
				}
			}
		}

		public static Vector3 At(List<float> xyz, int v) => new Vector3(xyz[v * 3], xyz[v * 3 + 1], xyz[v * 3 + 2]);
	}
}
