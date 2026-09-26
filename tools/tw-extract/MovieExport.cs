using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace TwExtract
{
	// Pre-rendered FMV/*.PSS movies -> movies/<NAME>.wav (the English track) + movies/<NAME>/f00001.jpg...
	// PSS = MPEG-2 program stream: video 512x288 25 fps; audio in private stream 0xBD, sub-stream 0xFF,
	// sub-header "ff a0 00 <track>". Each of the 5 language tracks opens with "SShd" (type 1 = s16 PCM,
	// rate, channels, interleave) + "SSbd" <size>, then L/R blocks of <interleave> bytes alternate.
	// The video is decoded by an external ffmpeg at extract time only (never linked or shipped).
	static class MovieExport
	{
		public static string Export(byte[] pss, string name, string outDir, string ffmpeg)
		{
			string dir = Path.Combine(outDir, "movies");
			Directory.CreateDirectory(dir);
			string pssPath = Path.Combine(dir, name + ".PSS");
			File.WriteAllBytes(pssPath, pss);
			string audio = Audio(pss, Path.Combine(dir, name + ".wav"), track: 0);

			string frames = Path.Combine(dir, name);
			if (Directory.Exists(frames))
			{
				Directory.Delete(frames, true);
			}
			Directory.CreateDirectory(frames);
			var psi = new ProcessStartInfo(ffmpeg, $"-v error -y -i \"{pssPath}\" -an -q:v 3 \"{Path.Combine(frames, "f%05d.jpg")}\"")
			{
				UseShellExecute = false,
				RedirectStandardError = true,
			};
			using (Process p = Process.Start(psi))
			{
				string err = p.StandardError.ReadToEnd();
				p.WaitForExit();
				if (p.ExitCode != 0)
				{
					throw new InvalidOperationException($"ffmpeg failed: {err}");
				}
			}
			File.Delete(pssPath);
			int count = Directory.GetFiles(frames, "*.jpg").Length;
			return $"{count} frames ({count / 25.0:F2} s at 25 fps), {audio}";
		}

		static string Audio(byte[] d, string wavPath, int track)
		{
			var body = new MemoryStream();
			int rate = 0, channels = 0, interleave = 0;
			for (int p = 0; p + 9 < d.Length;)
			{
				if (d[p] != 0 || d[p + 1] != 0 || d[p + 2] != 1)
				{
					p++;
					continue;
				}
				byte id = d[p + 3];
				if (id == 0xBA) // pack header (MPEG-2: 14 bytes + stuffing)
				{
					p += 14 + (d[p + 13] & 7);
					continue;
				}
				if (id < 0xBB)
				{
					p++;
					continue;
				}
				int len = d[p + 4] << 8 | d[p + 5];
				if (id == 0xBD)
				{
					int payload = p + 9 + d[p + 8];
					int end = p + 6 + len;
					if (d[payload] == 0xFF && d[payload + 3] == track)
					{
						int s = payload + 4;
						if (Encoding.ASCII.GetString(d, s, 4) == "SShd")
						{
							rate = BitConverter.ToInt32(d, s + 12);
							channels = BitConverter.ToInt32(d, s + 16);
							interleave = BitConverter.ToInt32(d, s + 20);
							s += 8 + BitConverter.ToInt32(d, s + 4);
							s += 8; // "SSbd" + size
						}
						body.Write(d, s, end - s);
					}
				}
				p += 6 + len;
			}
			if (channels != 2 || interleave <= 0)
			{
				throw new InvalidDataException($"unexpected PSS audio header: {channels} ch, interleave {interleave}");
			}
			// Blocks of L then R -> interleaved s16 frames.
			byte[] raw = body.ToArray();
			int pairs = raw.Length / (2 * interleave);
			var pcm = new byte[pairs * 2 * interleave];
			int o = 0;
			for (int b = 0; b < pairs; b++)
			{
				int l = b * 2 * interleave, r = l + interleave;
				for (int i = 0; i < interleave; i += 2)
				{
					pcm[o++] = raw[l + i]; pcm[o++] = raw[l + i + 1];
					pcm[o++] = raw[r + i]; pcm[o++] = raw[r + i + 1];
				}
			}
			using (var w = new BinaryWriter(File.Create(wavPath)))
			{
				w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + pcm.Length); w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
				w.Write(16); w.Write((short)1); w.Write((short)2); w.Write(rate); w.Write(rate * 4); w.Write((short)4); w.Write((short)16);
				w.Write(Encoding.ASCII.GetBytes("data")); w.Write(pcm.Length); w.Write(pcm);
			}
			return $"{pcm.Length / (rate * 4.0):F2} s audio at {rate} Hz";
		}
	}
}
