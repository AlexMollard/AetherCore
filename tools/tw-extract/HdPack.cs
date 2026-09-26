using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using Twinsanity;
using Path = System.IO.Path;

namespace TwExtract
{
	// HD-pack matching, perceptual (PCSX2's exact replacement hash is NOT reproducible from disc data:
	// the texhash covers the texture's GS memory blocks at runtime, and our reconstruction - the same
	// EzSwizzle round trip Pixels.Decode validates pixel-correct - hashes to values that appear nowhere
	// in the pack's 547 texhashes, so runtime state we cannot see (DBW/CSA/TBW as the game sets them,
	// runtime-built CLUTs) must enter the hash. Fallback: downscale each pack PNG and the native texture
	// to a 32x32 thumb, keep aspect-compatible pairs, and accept the lowest mean-abs-diff under a tight
	// threshold, one pack file per native texture. The pack's contract (a PCSX2 replacement) makes every
	// file cover the same UV space as its native, so an accepted match drops in over the native PNG.
	static class HdPack
	{
		const int Thumb = 32;
		// Thumbs are alpha-premultiplied, so the arbitrary colour a PS2 texture keeps in its transparent
		// texels never counts. Measured on Startup's UI: true pairs land at 0.7-7.9 per channel, the
		// runner-up at 5-37, so a match must be small and clearly ahead of the next candidate.
		const double Threshold = 8.5;
		const double Separation = 0.5;
		const double SureMatch = 1.0; // near-identical variants (e.g. digit tiles) can sit close together

		sealed class Candidate
		{
			public ZipArchiveEntry Entry;
			public string Folder;
			public double Aspect;   // w/h of the full pack image
			public byte[] Thumb;    // 32*32*4 RGBA thumb, colour premultiplied by alpha
		}

		static readonly List<Candidate> s_pack = new();
		static readonly HashSet<string> s_used = new(StringComparer.OrdinalIgnoreCase);
		static readonly BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;

		public static int Matched { get; private set; }
		public static bool Loaded { get; private set; }
		static readonly Dictionary<string, (int total, int matched)> s_folders = new(StringComparer.OrdinalIgnoreCase);
		static readonly List<string> s_unmatchedUi = new();
		static readonly HashSet<string> s_seenUi = new(StringComparer.OrdinalIgnoreCase);
		// Diagnostics: native texture -> best diff, for threshold tuning (printed in verbose report).
		public static bool Verbose;

		public static void Load(string zipPath)
		{
			// Kept open for the whole run: matched entries are decoded at full size on demand.
			var zip = ZipFile.OpenRead(zipPath);
			foreach (var e in zip.Entries)
			{
				if (!e.FullName.EndsWith(".png", StringComparison.OrdinalIgnoreCase) || e.FullName.Split('/').Length < 3)
				{
					continue;
				}
				var (w, h, rgba) = Decode(e);
				s_pack.Add(new Candidate
				{
					Entry = e,
					Folder = e.FullName.Replace('\\', '/').Split('/')[1],
					Aspect = (double)w / h,
					Thumb = Shrink(rgba, w, h, Thumb, Thumb),
				});
				string folder = e.FullName.Replace('\\', '/').Split('/')[1];
				var (total, _) = s_folders.TryGetValue(folder, out var f) ? f : (0, 0);
				s_folders[folder] = (total + 1, 0);
			}
			Loaded = true;
		}

		// PCSX2 dumps a texture in GS memory order, which is upside down against our decoded (upright)
		// PNGs, and keeps the GS 0..0x80 colour and alpha range. Pixels.Decode keeps colour there (Brighten
		// doubles it for UI) but widens alpha, so the rows are flipped and alpha widened the same way.
		static (int, int, byte[]) Decode(ZipArchiveEntry entry)
		{
			using var src = entry.Open();
			using var bmp = new Bitmap(src);
			int w = bmp.Width, h = bmp.Height;
			var bgra = new byte[w * h * 4];
			var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
			for (int y = 0; y < h; y++)
			{
				System.Runtime.InteropServices.Marshal.Copy(data.Scan0 + (h - 1 - y) * data.Stride, bgra, y * w * 4, w * 4);
			}
			bmp.UnlockBits(data);
			var rgba = new byte[w * h * 4];
			for (int i = 0; i < w * h; i++)
			{
				rgba[i * 4 + 0] = bgra[i * 4 + 2];
				rgba[i * 4 + 1] = bgra[i * 4 + 1];
				rgba[i * 4 + 2] = bgra[i * 4 + 0];
				rgba[i * 4 + 3] = (byte)Math.Min(bgra[i * 4 + 3] * 2, 255); // GS 0..0x80 alpha, as Pixels.Decode widens it
			}
			return (w, h, rgba);
		}

		// Area-average resample of an RGBA image to dw x dh.
		static byte[] Shrink(byte[] rgba, int w, int h, int dw, int dh)
		{
			var dst = new byte[dw * dh * 4];
			for (int y = 0; y < dh; y++)
			{
				int y0 = y * h / dh, y1 = Math.Max(y0 + 1, (y + 1) * h / dh);
				for (int x = 0; x < dw; x++)
				{
					int x0 = x * w / dw, x1 = Math.Max(x0 + 1, (x + 1) * w / dw);
					int r = 0, g = 0, b = 0, a = 0, n = 0;
					for (int yy = y0; yy < y1; yy++)
					{
						for (int xx = x0; xx < x1; xx++)
						{
							int p = (yy * w + xx) * 4;
							int alpha = rgba[p + 3];
							r += rgba[p] * alpha / 255;
							g += rgba[p + 1] * alpha / 255;
							b += rgba[p + 2] * alpha / 255;
							a += rgba[p + 3];
							n++;
						}
					}
					int o = (y * dw + x) * 4;
					dst[o] = (byte)(r / n);
					dst[o + 1] = (byte)(g / n);
					dst[o + 2] = (byte)(b / n);
					dst[o + 3] = (byte)(a / n);
				}
			}
			return dst;
		}

		static double Diff(byte[] a, byte[] b)
		{
			long sum = 0;
			for (int i = 0; i < a.Length; i++)
			{
				sum += Math.Abs(a[i] - b[i]);
			}
			return sum / (double)a.Length;
		}

		// The pack's HD pixels for this native texture (pre-brighten), or false when nothing matches.
		// Brighten mirrors the native UI path (sprite art is drawn with a 2x vertex colour).
		public static bool Replacement(Texture t, byte[] native, bool brighten, bool ui, string uiKey, out byte[] rgba, out int w, out int h)
		{
			rgba = null;
			w = h = 0;
			if (!Loaded || native == null)
			{
				return false;
			}
			var thumb = Shrink(native, t.Width, t.Height, Thumb, Thumb);
			double aspect = (double)t.Width / t.Height;
			Candidate best = null;
			double bestDiff = double.MaxValue, second = double.MaxValue;
			foreach (var c in s_pack)
			{
				if (Math.Abs(Math.Log(aspect / c.Aspect)) > 0.08)
				{
					continue;
				}
				double d = Diff(thumb, c.Thumb);
				if (d < bestDiff)
				{
					second = bestDiff;
					bestDiff = d;
					best = c;
				}
				else if (d < second)
				{
					second = d;
				}
			}
			if (Verbose && ui && s_seenUi.Add("v:" + uiKey))
			{
				Console.WriteLine($"hd-pack: {uiKey} best={bestDiff:F2} second={second:F2} {(best?.Folder ?? "-")}");
			}
			if (best == null || bestDiff > Threshold || (bestDiff > SureMatch && bestDiff > second * Separation))
			{
				if (ui && s_seenUi.Add(uiKey))
				{
					s_unmatchedUi.Add($"{uiKey} [best diff {bestDiff:F2}, next {second:F2}]");
				}
				return false;
			}
			(w, h, rgba) = Decode(best.Entry);
			if (brighten)
			{
				Pixels.Brighten(rgba);
			}
			Matched++;
			// One pack file can stand in for several identical natives (e.g. the same icon in two
			// archives); the folder rate counts pack files used, not replacements written.
			if (s_used.Add(best.Entry.FullName))
			{
				var (total, matched) = s_folders.TryGetValue(best.Folder, out var f) ? f : (0, 0);
				s_folders[best.Folder] = (total, matched + 1);
			}
			return true;
		}

		// Per-folder match rate and the UI textures the pack does not cover.
		public static string Report()
		{
			var sb = new System.Text.StringBuilder();
			sb.AppendLine($"hd-pack: {Matched} extracted textures replaced (perceptual match)");
			foreach (var kv in s_folders)
			{
				sb.AppendLine($"  {kv.Key}: {kv.Value.matched}/{kv.Value.total}");
			}
			if (s_unmatchedUi.Count > 0)
			{
				sb.AppendLine($"  unmatched UI textures ({s_unmatchedUi.Count}):");
				foreach (string u in s_unmatchedUi)
				{
					sb.AppendLine($"    {u}");
				}
			}
			return sb.ToString();
		}
	}
}
