using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace TwExtract
{
	// One triangle-list primitive under construction. Attribute lists are flat (xyz, uv, rgba, ...).
	sealed class Prim
	{
		public readonly List<float> Pos = new List<float>();
		public readonly List<float> Nrm = new List<float>();
		public readonly List<float> Uv = new List<float>();
		public readonly List<float> Col = new List<float>();
		public readonly List<ushort> Joints = new List<ushort>();
		public readonly List<float> Weights = new List<float>();
		public readonly List<uint> Idx = new List<uint>();
		public int VertexCount => Pos.Count / 3;

		// Same attribute layout required (both vertex-coloured or both skinned), which the callers guarantee.
		public void Append(Prim o)
		{
			uint offset = (uint)VertexCount;
			Pos.AddRange(o.Pos); Nrm.AddRange(o.Nrm); Uv.AddRange(o.Uv); Col.AddRange(o.Col);
			Joints.AddRange(o.Joints); Weights.AddRange(o.Weights);
			Idx.AddRange(o.Idx.Select(i => i + offset));
		}
	}

	// Just enough glTF 2.0 for static meshes, skeletons and skins: one .gltf + one .bin, external PNGs.
	sealed class Gltf
	{
		readonly MemoryStream m_bin = new MemoryStream();
		readonly List<object> m_views = new List<object>(), m_accessors = new List<object>(), m_meshes = new List<object>();
		readonly List<object> m_materials = new List<object>(), m_textures = new List<object>(), m_images = new List<object>();
		readonly List<object> m_skins = new List<object>();
		readonly Dictionary<string, int> m_imageByUri = new Dictionary<string, int>();
		public readonly List<Dictionary<string, object>> Nodes = new List<Dictionary<string, object>>();
		public readonly List<int> SceneRoots = new List<int>();

		public int AddNode(Dictionary<string, object> node)
		{
			Nodes.Add(node);
			return Nodes.Count - 1;
		}

		public static void AddChild(Dictionary<string, object> parent, int child)
		{
			if (!parent.TryGetValue("children", out var list))
			{
				parent["children"] = list = new List<int>();
			}
			((List<int>)list).Add(child);
		}

		public int AddMaterial(Dictionary<string, object> material)
		{
			m_materials.Add(material);
			return m_materials.Count - 1;
		}

		public int TextureFor(string uri)
		{
			if (!m_imageByUri.TryGetValue(uri, out int image))
			{
				m_images.Add(new Dictionary<string, object> { ["uri"] = uri });
				m_textures.Add(new Dictionary<string, object> { ["source"] = m_images.Count - 1 });
				image = m_imageByUri[uri] = m_images.Count - 1;
			}
			return image; // one texture per image, same index
		}

		// Returns the mesh index, or -1 when every primitive is empty.
		public int AddMesh(string name, IEnumerable<(Prim Prim, int Material)> prims)
		{
			var list = new List<object>();
			foreach (var (p, material) in prims)
			{
				if (p.Idx.Count == 0)
				{
					continue;
				}
				var attrs = new Dictionary<string, object> { ["POSITION"] = Floats(p.Pos, 3, "VEC3", true) };
				if (p.Nrm.Count == p.Pos.Count)
				{
					attrs["NORMAL"] = Floats(p.Nrm, 3, "VEC3", false);
				}
				if (p.Uv.Count == p.VertexCount * 2)
				{
					attrs["TEXCOORD_0"] = Floats(p.Uv, 2, "VEC2", false);
				}
				if (p.Col.Count == p.VertexCount * 4)
				{
					attrs["COLOR_0"] = Floats(p.Col, 4, "VEC4", false);
				}
				if (p.Joints.Count == p.VertexCount * 4)
				{
					attrs["JOINTS_0"] = UShorts(p.Joints);
					attrs["WEIGHTS_0"] = Floats(p.Weights, 4, "VEC4", false);
				}
				var prim = new Dictionary<string, object> { ["attributes"] = attrs, ["indices"] = Indices(p.Idx), ["mode"] = 4 };
				if (material >= 0)
				{
					prim["material"] = material;
				}
				list.Add(prim);
			}
			if (list.Count == 0)
			{
				return -1;
			}
			m_meshes.Add(new Dictionary<string, object> { ["name"] = name, ["primitives"] = list });
			return m_meshes.Count - 1;
		}

		public int AddSkin(List<int> joints, float[] inverseBindMatrices, int skeletonRoot)
		{
			int ibm = Accessor(View(ToBytes(inverseBindMatrices), null), 5126, joints.Count, "MAT4", null, null);
			m_skins.Add(new Dictionary<string, object> { ["joints"] = joints, ["inverseBindMatrices"] = ibm, ["skeleton"] = skeletonRoot });
			return m_skins.Count - 1;
		}

		public void Save(string gltfPath)
		{
			string binName = Path.GetFileNameWithoutExtension(gltfPath) + ".bin";
			if (m_bin.Length > 0)
			{
				File.WriteAllBytes(Path.Combine(Path.GetDirectoryName(gltfPath), binName), m_bin.ToArray());
			}
			File.WriteAllText(gltfPath, RenderJson(binName));
		}

		// Identity of the model independent of where it is written (the .bin name is the only path in it).
		public string ContentHash() => Hash.Of(Encoding.UTF8.GetBytes(RenderJson("model.bin")), m_bin.ToArray());

		string RenderJson(string binName)
		{
			var root = new Dictionary<string, object>
			{
				["asset"] = new Dictionary<string, object> { ["version"] = "2.0", ["generator"] = "AetherCore tw-extract" },
				["scene"] = 0,
				["scenes"] = new List<object> { new Dictionary<string, object> { ["nodes"] = SceneRoots } },
				["nodes"] = Nodes,
			};
			void Put(string key, List<object> list)
			{
				if (list.Count > 0)
				{
					root[key] = list;
				}
			}
			Put("meshes", m_meshes);
			Put("materials", m_materials);
			Put("textures", m_textures);
			Put("images", m_images);
			Put("skins", m_skins);
			Put("accessors", m_accessors);
			Put("bufferViews", m_views);
			if (m_bin.Length > 0)
			{
				root["buffers"] = new List<object> { new Dictionary<string, object> { ["uri"] = binName, ["byteLength"] = m_bin.Length } };
			}
			var sb = new StringBuilder();
			Json.Write(sb, root);
			return sb.ToString();
		}

		int Floats(List<float> data, int comps, string type, bool minMax)
		{
			float[] min = null, max = null;
			if (minMax)
			{
				min = Enumerable.Repeat(float.MaxValue, comps).ToArray();
				max = Enumerable.Repeat(float.MinValue, comps).ToArray();
				for (int i = 0; i < data.Count; i++)
				{
					min[i % comps] = Math.Min(min[i % comps], data[i]);
					max[i % comps] = Math.Max(max[i % comps], data[i]);
				}
			}
			return Accessor(View(ToBytes(data.ToArray()), 34962), 5126, data.Count / comps, type, min, max);
		}

		int UShorts(List<ushort> data)
		{
			var bytes = new byte[data.Count * 2];
			Buffer.BlockCopy(data.ToArray(), 0, bytes, 0, bytes.Length);
			return Accessor(View(bytes, 34962), 5123, data.Count / 4, "VEC4", null, null);
		}

		int Indices(List<uint> idx)
		{
			var bytes = new byte[idx.Count * 4];
			Buffer.BlockCopy(idx.ToArray(), 0, bytes, 0, bytes.Length);
			return Accessor(View(bytes, 34963), 5125, idx.Count, "SCALAR", null, null);
		}

		static byte[] ToBytes(float[] data)
		{
			var bytes = new byte[data.Length * 4];
			Buffer.BlockCopy(data, 0, bytes, 0, bytes.Length);
			return bytes;
		}

		int View(byte[] bytes, int? target)
		{
			while (m_bin.Length % 4 != 0)
			{
				m_bin.WriteByte(0);
			}
			var view = new Dictionary<string, object> { ["buffer"] = 0, ["byteOffset"] = m_bin.Length, ["byteLength"] = bytes.Length };
			if (target.HasValue)
			{
				view["target"] = target.Value;
			}
			m_bin.Write(bytes, 0, bytes.Length);
			m_views.Add(view);
			return m_views.Count - 1;
		}

		int Accessor(int view, int componentType, int count, string type, float[] min, float[] max)
		{
			var a = new Dictionary<string, object> { ["bufferView"] = view, ["componentType"] = componentType, ["count"] = count, ["type"] = type };
			if (min != null)
			{
				a["min"] = min;
				a["max"] = max;
			}
			m_accessors.Add(a);
			return m_accessors.Count - 1;
		}
	}

	static class Json
	{
		public static void Write(StringBuilder sb, object v)
		{
			switch (v)
			{
				case null: sb.Append("null"); break;
				case string s: String(sb, s); break;
				case bool b: sb.Append(b ? "true" : "false"); break;
				case float f: sb.Append(!float.IsNaN(f) && !float.IsInfinity(f) ? f.ToString("R", CultureInfo.InvariantCulture) : "0"); break;
				case double d: sb.Append(!double.IsNaN(d) && !double.IsInfinity(d) ? d.ToString("R", CultureInfo.InvariantCulture) : "0"); break;
				case int or long or uint or ushort: sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture)); break;
				case IDictionary dict:
					sb.Append('{');
					bool first = true;
					foreach (DictionaryEntry e in dict)
					{
						if (!first)
						{
							sb.Append(',');
						}
						first = false;
						String(sb, (string)e.Key);
						sb.Append(':');
						Write(sb, e.Value);
					}
					sb.Append('}');
					break;
				case IEnumerable list:
					sb.Append('[');
					bool firstItem = true;
					foreach (var item in list)
					{
						if (!firstItem)
						{
							sb.Append(',');
						}
						firstItem = false;
						Write(sb, item);
					}
					sb.Append(']');
					break;
				default: throw new InvalidOperationException($"Json: unsupported {v.GetType()}");
			}
		}

		static void String(StringBuilder sb, string s)
		{
			sb.Append('"');
			foreach (char c in s)
			{
				switch (c)
				{
					case '"': sb.Append("\\\""); break;
					case '\\': sb.Append("\\\\"); break;
					default:
						if (c < 0x20)
						{
							sb.Append("\\u").Append(((int)c).ToString("x4"));
						}
						else
						{
							sb.Append(c);
						}
						break;
				}
			}
			sb.Append('"');
		}
	}
}
