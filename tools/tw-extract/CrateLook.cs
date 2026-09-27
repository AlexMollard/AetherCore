using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace TwExtract
{
	// Crate look upgrade (logs/look/CrateLook/README.md). Every crate object's material gets surface
	// maps derived from its own albedo, so the crates keep the original's (or the HD pack's) art and
	// colours and only gain relief and a real surface response:
	//   <stem>_cn.png   tangent-space normal (X along +U, Y along +V - gltf_mesh.slang's
	//                   ObjectLitNormal builds that frame from screen derivatives, crates have no
	//                   TANGENT stream), from the albedo's luminance: planks and rivets stand
	//                   proud, grooves and grain sink.
	//   <stem>_corm.png glTF ORM: R cavity occlusion (ambient only), G roughness, B metalness.
	//                   Grey unsaturated texels (iron frames, rivets, straps) are metal; bright
	//                   yellow-green texels (TNT, nitro, arrow, C, ? markings) are glossier paint;
	//                   the rest is wood.
	//   <stem>_cw.png   the albedo with edge wear: the outer band of a crate face (the crate's
	//                   edges) is scuffed lighter in noisy patches, metal edges polish. Face art
	//                   only (>= 128 px); the small plank-fragment textures keep their albedo.
	//   <stem>_ce.png   nitro and TNT: the emissive mask - the yellow-green paint (letters, digits,
	//                   stripes) fully, a nitro's green body faintly. It keeps the TNT and nitro
	//                   markings readable in shade and at range; CrateFx pulses the nitro's with its
	//                   idle hop.
	// Deterministic: the same albedo always gives the same maps, so a re-extract (with or without
	// the HD pack) regenerates them byte-for-byte.
	static class CrateLook
	{
		// Knobs (README "Knobs").
		const float NormalStrength = 5.0f;   // relief: luminance slope -> normal tilt, at 256 px
		const float CavityGain = 2.2f;       // how hard grooves darken the ambient
		const float CavityMax = 0.55f;       // deepest ambient occlusion a groove gets
		const float RoughWood = 0.72f, RoughGroove = 0.92f, RoughPaint = 0.42f, RoughMetal = 0.34f, RoughWornMetal = 0.2f;
		const float WearBand = 0.045f;       // edge wear band, fraction of the face
		const float WearLift = 0.22f;        // how much lighter a scuffed edge gets
		public const float GlowBase = 0.12f; // baked emissive factor; CrateFx pulses a nitro above it

		public static bool IsCrate(string objectName) => objectName.IndexOf("CRATE", StringComparison.Ordinal) >= 0;
		public static bool IsGlow(string objectName) => objectName.IndexOf("NITRO", StringComparison.Ordinal) >= 0 || objectName.IndexOf("TNT", StringComparison.Ordinal) >= 0;

		public sealed class Maps
		{
			public string Albedo, Normal, Orm, Emissive;
		}

		static readonly Dictionary<string, Maps> s_made = new Dictionary<string, Maps>();

		// Writes the maps for texturesDir/albedo (once per run) and returns their file names.
		public static Maps Make(string texturesDir, string albedo, bool glow)
		{
			string key = albedo + (glow ? "|glow" : "");
			if (s_made.TryGetValue(key, out var made))
			{
				return made;
			}
			string stem = Path.GetFileNameWithoutExtension(albedo);
			var (w, h, rgba) = LoadPng(Path.Combine(texturesDir, albedo));
			var maps = new Maps { Albedo = albedo, Normal = stem + "_cn.png", Orm = stem + "_corm.png" };
			int n = w * h;
			float res = w / 256.0f;
			var lum = new float[n];
			var sat = new float[n];
			var hue = new float[n];
			for (int i = 0; i < n; i++)
			{
				float r = rgba[i * 4] / 255f, g = rgba[i * 4 + 1] / 255f, b = rgba[i * 4 + 2] / 255f;
				float mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
				lum[i] = 0.299f * r + 0.587f * g + 0.114f * b;
				sat[i] = mx > 1e-4f ? (mx - mn) / mx : 0f;
				hue[i] = Hue(r, g, b, mx, mn);
			}
			var hs = Blur(lum, w, h, Math.Max(1, (int)Math.Round(res)));
			var hl = Blur(lum, w, h, Math.Max(2, w / 24));
			var normal = new byte[n * 4];
			var orm = new byte[n * 4];
			var worn = (byte[])rgba.Clone();
			var emis = glow ? new byte[n * 4] : null;
			bool faceArt = w >= 128 && h >= 128;
			float strength = NormalStrength * res;
			for (int y = 0; y < h; y++)
			{
				for (int x = 0; x < w; x++)
				{
					int i = y * w + x;
					float gx = (hs[y * w + Math.Min(x + 1, w - 1)] - hs[y * w + Math.Max(x - 1, 0)]) * 0.5f;
					float gy = (hs[Math.Min(y + 1, h - 1) * w + x] - hs[Math.Max(y - 1, 0) * w + x]) * 0.5f;
					float nx = -gx * strength, ny = -gy * strength, nz = 1f;
					float inv = 1f / (float)Math.Sqrt(nx * nx + ny * ny + nz * nz);
					normal[i * 4] = Byte(nx * inv * 0.5f + 0.5f);
					normal[i * 4 + 1] = Byte(ny * inv * 0.5f + 0.5f);
					normal[i * 4 + 2] = Byte(nz * inv * 0.5f + 0.5f);
					normal[i * 4 + 3] = 255;

					float cavity = Clamp01((hl[i] - hs[i]) * CavityGain);
					float metal = Smooth(0.28f, 0.14f, sat[i]) * Smooth(0.10f, 0.22f, lum[i]);
					bool yellowGreen = hue[i] >= 45f && hue[i] <= 170f;
					float paint = yellowGreen ? Smooth(0.45f, 0.65f, sat[i]) * Smooth(0.30f, 0.45f, lum[i]) : 0f;
					float wear = 0f;
					if (faceArt)
					{
						float d = Math.Min(Math.Min(x, w - 1 - x) / (float)w, Math.Min(y, h - 1 - y) / (float)h);
						float band = Smooth(WearBand, 0f, d);
						// Patchy scuffs, not a uniform stripe: two octaves of value noise.
						float noise = 0.65f * ValueNoise(x / (6f * res), y / (6f * res)) + 0.35f * ValueNoise(x / (2f * res) + 17f, y / (2f * res) + 5f);
						wear = band * Smooth(0.35f, 0.7f, noise);
					}
					float rough = Lerp(Lerp(RoughWood, RoughGroove, cavity), RoughPaint, paint);
					rough = Lerp(rough, Lerp(RoughMetal, RoughWornMetal, wear), metal);
					orm[i * 4] = Byte(1f - CavityMax * cavity * (1f - wear));
					orm[i * 4 + 1] = Byte(rough);
					orm[i * 4 + 2] = Byte(metal);
					orm[i * 4 + 3] = 255;
					if (wear > 0f)
					{
						// Scuffed wood lightens towards its own raw tone; metal edges brighten.
						for (int c = 0; c < 3; c++)
						{
							float v = rgba[i * 4 + c] / 255f;
							worn[i * 4 + c] = Byte(v + (1f - v) * WearLift * wear * (0.6f + 0.4f * v));
						}
					}
					if (emis != null)
					{
						float green = yellowGreen ? Smooth(0.3f, 0.6f, sat[i]) : 0f;
						float k = green * (0.35f + 0.65f * paint);
						for (int c = 0; c < 3; c++)
						{
							emis[i * 4 + c] = Byte(rgba[i * 4 + c] / 255f * k);
						}
						emis[i * 4 + 3] = 255;
					}
				}
			}
			SavePng(Path.Combine(texturesDir, maps.Normal), w, h, normal);
			SavePng(Path.Combine(texturesDir, maps.Orm), w, h, orm);
			if (faceArt)
			{
				maps.Albedo = stem + "_cw.png";
				SavePng(Path.Combine(texturesDir, maps.Albedo), w, h, worn);
			}
			if (emis != null)
			{
				maps.Emissive = stem + "_ce.png";
				SavePng(Path.Combine(texturesDir, maps.Emissive), w, h, emis);
			}
			return s_made[key] = maps;
		}

		// Decorates a glTF material (Export.Material) with the maps; texture(uri) registers an image.
		public static void Apply(Dictionary<string, object> gltfMat, Maps maps, string texturesRel, Func<string, int> texture, bool glow)
		{
			var pbr = (Dictionary<string, object>)gltfMat["pbrMetallicRoughness"];
			pbr["baseColorTexture"] = new Dictionary<string, object> { ["index"] = texture(texturesRel + "/" + maps.Albedo) };
			pbr["metallicFactor"] = 1f;  // the ORM texture carries the values
			pbr["roughnessFactor"] = 1f;
			int orm = texture(texturesRel + "/" + maps.Orm);
			pbr["metallicRoughnessTexture"] = new Dictionary<string, object> { ["index"] = orm };
			gltfMat["occlusionTexture"] = new Dictionary<string, object> { ["index"] = orm };
			gltfMat["normalTexture"] = new Dictionary<string, object> { ["index"] = texture(texturesRel + "/" + maps.Normal) };
			if (glow && maps.Emissive != null)
			{
				gltfMat["emissiveTexture"] = new Dictionary<string, object> { ["index"] = texture(texturesRel + "/" + maps.Emissive) };
				gltfMat["emissiveFactor"] = new[] { GlowBase, GlowBase, GlowBase };
			}
		}

		static float Hue(float r, float g, float b, float mx, float mn)
		{
			float d = mx - mn;
			if (d < 1e-5f)
			{
				return -1f;
			}
			float hh = mx == r ? (g - b) / d : mx == g ? 2f + (b - r) / d : 4f + (r - g) / d;
			hh *= 60f;
			return hh < 0f ? hh + 360f : hh;
		}

		// Separable box blur, clamped at the edges.
		static float[] Blur(float[] src, int w, int h, int r)
		{
			var tmp = new float[src.Length];
			var dst = new float[src.Length];
			for (int y = 0; y < h; y++)
			{
				for (int x = 0; x < w; x++)
				{
					float s = 0f;
					for (int k = -r; k <= r; k++)
					{
						s += src[y * w + Math.Min(Math.Max(x + k, 0), w - 1)];
					}
					tmp[y * w + x] = s / (2 * r + 1);
				}
			}
			for (int y = 0; y < h; y++)
			{
				for (int x = 0; x < w; x++)
				{
					float s = 0f;
					for (int k = -r; k <= r; k++)
					{
						s += tmp[Math.Min(Math.Max(y + k, 0), h - 1) * w + x];
					}
					dst[y * w + x] = s / (2 * r + 1);
				}
			}
			return dst;
		}

		static float ValueNoise(float x, float y)
		{
			int x0 = (int)Math.Floor(x), y0 = (int)Math.Floor(y);
			float fx = x - x0, fy = y - y0;
			fx = fx * fx * (3f - 2f * fx);
			fy = fy * fy * (3f - 2f * fy);
			float a = Lattice(x0, y0), b = Lattice(x0 + 1, y0), c = Lattice(x0, y0 + 1), d = Lattice(x0 + 1, y0 + 1);
			return Lerp(Lerp(a, b, fx), Lerp(c, d, fx), fy);
		}

		static float Lattice(int x, int y)
		{
			uint hsh = (uint)(x * 374761393 + y * 668265263);
			hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
			return ((hsh ^ (hsh >> 16)) & 0xFFFF) / 65535f;
		}

		static float Lerp(float a, float b, float t) => a + (b - a) * t;
		static float Clamp01(float v) => v < 0f ? 0f : v > 1f ? 1f : v;
		static float Smooth(float e0, float e1, float v)
		{
			float t = Clamp01((v - e0) / (e1 - e0));
			return t * t * (3f - 2f * t);
		}
		static byte Byte(float v) => (byte)Math.Round(Clamp01(v) * 255f);

		static (int, int, byte[]) LoadPng(string path)
		{
			using (var bmp = new Bitmap(path))
			{
				int w = bmp.Width, h = bmp.Height;
				var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
				var bgra = new byte[w * h * 4];
				for (int y = 0; y < h; y++)
				{
					Marshal.Copy(data.Scan0 + y * data.Stride, bgra, y * w * 4, w * 4);
				}
				bmp.UnlockBits(data);
				var rgba = new byte[bgra.Length];
				for (int i = 0; i < w * h; i++)
				{
					rgba[i * 4] = bgra[i * 4 + 2];
					rgba[i * 4 + 1] = bgra[i * 4 + 1];
					rgba[i * 4 + 2] = bgra[i * 4];
					rgba[i * 4 + 3] = bgra[i * 4 + 3];
				}
				return (w, h, rgba);
			}
		}

		static void SavePng(string path, int w, int h, byte[] rgba) => Pixels.SavePng(path, w, h, rgba);

		// tw-extract --crate-look <assets dir> <albedo.png> [glow]: regenerate one texture's maps from
		// an already-extracted albedo (look-dev iteration without the disc).
		public static int Cli(string assetsDir, string albedo, bool glow)
		{
			var m = Make(Path.Combine(assetsDir, "textures"), albedo, glow);
			Console.WriteLine($"{albedo}: {m.Albedo} {m.Normal} {m.Orm} {m.Emissive}");
			return 0;
		}
	}
}
