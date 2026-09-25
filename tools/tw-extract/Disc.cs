using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace TwExtract
{
	// Reads the PAL disc directly: ISO9660 directory -> /CRASH6/CRASH.BH (name, offset, size table) -> CRASH.BD.
	// Mirrors CrashModded's tools/isotools.py (iso_files / parse_bh / archive_file / pcsx2_crc).
	sealed class Disc : IDisposable
	{
		const int Sector = 2048;
		// PCSX2 CRC of the untouched PAL executable; a modded disc has a different one and is refused.
		public const string OriginalCrc = "1510E1D1";

		readonly FileStream m_iso;
		readonly long m_bdOffset;
		public readonly List<(string Name, uint Offset, uint Size)> Entries = new List<(string, uint, uint)>();
		public readonly string Crc;

		public Disc(string isoPath)
		{
			m_iso = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read);
			var files = IsoFiles();
			Crc = Pcsx2Crc(ReadIsoFile(files, "/SLES_525.68"));
			m_bdOffset = (long)files["/CRASH6/CRASH.BD"].Lba * Sector;
			var bh = ReadIsoFile(files, "/CRASH6/CRASH.BH");
			for (int p = 4; p < bh.Length;)
			{
				int n = BitConverter.ToInt32(bh, p);
				string name = Encoding.ASCII.GetString(bh, p + 4, n);
				Entries.Add((name, BitConverter.ToUInt32(bh, p + 4 + n), BitConverter.ToUInt32(bh, p + 8 + n)));
				p += 12 + n;
			}
		}

		public byte[] Read((string Name, uint Offset, uint Size) entry)
		{
			var data = new byte[entry.Size];
			m_iso.Position = m_bdOffset + entry.Offset;
			ReadExactly(data);
			return data;
		}

		public void Dispose() => m_iso.Dispose();

		Dictionary<string, (uint Lba, uint Size)> IsoFiles()
		{
			var outFiles = new Dictionary<string, (uint, uint)>(StringComparer.OrdinalIgnoreCase);
			var pvd = ReadAt(16L * Sector, Sector);
			Walk(BitConverter.ToUInt32(pvd, 156 + 2), BitConverter.ToUInt32(pvd, 156 + 10), "", outFiles);
			return outFiles;
		}

		void Walk(uint lba, uint size, string path, Dictionary<string, (uint, uint)> outFiles)
		{
			var data = ReadAt((long)lba * Sector, (int)size);
			for (int i = 0; i < data.Length;)
			{
				int n = data[i];
				if (n == 0)
				{
					i = (i / Sector + 1) * Sector;
					continue;
				}
				uint elba = BitConverter.ToUInt32(data, i + 2), esz = BitConverter.ToUInt32(data, i + 10);
				int nameLen = data[i + 32];
				if (!(nameLen == 1 && data[i + 33] <= 1))
				{
					string p = path + "/" + Encoding.ASCII.GetString(data, i + 33, nameLen).Split(';')[0];
					if ((data[i + 25] & 2) != 0)
					{
						Walk(elba, esz, p, outFiles);
					}
					else
					{
						outFiles[p] = (elba, esz);
					}
				}
				i += n;
			}
		}

		byte[] ReadIsoFile(Dictionary<string, (uint Lba, uint Size)> files, string path)
		{
			var (lba, size) = files[path];
			return ReadAt((long)lba * Sector, (int)size);
		}

		byte[] ReadAt(long offset, int size)
		{
			var buf = new byte[size];
			m_iso.Position = offset;
			ReadExactly(buf);
			return buf;
		}

		void ReadExactly(byte[] buf)
		{
			for (int got = 0; got < buf.Length;)
			{
				int r = m_iso.Read(buf, got, buf.Length - got);
				if (r <= 0)
				{
					throw new EndOfStreamException("Disc image is truncated.");
				}
				got += r;
			}
		}

		// PCSX2's game CRC: XOR of the executable's 32-bit little-endian words.
		static string Pcsx2Crc(byte[] elf)
		{
			uint c = 0;
			for (int i = 0; i + 4 <= elf.Length; i += 4)
			{
				c ^= BitConverter.ToUInt32(elf, i);
			}
			return c.ToString("X8");
		}
	}
}
