#include "GltfAsset.hpp"
#include <nlohmann/json.hpp>
#include <cstring>
#include <cmath>

namespace aether::assets
{
	namespace
	{
		using json = nlohmann::json;

		bool Number(const json& j, const char* key, float& out)
		{
			auto it = j.find(key);
			if (it == j.end() || !it->is_number())
			{
				return false;
			}
			out = it->get<float>();
			return std::isfinite(out);
		}

		bool Integer(const json& j, const char* key, std::int32_t& out)
		{
			auto it = j.find(key);
			if (it == j.end() || !it->is_number_integer())
			{
				return false;
			}
			auto n = it->get<std::int64_t>();
			if (n < INT32_MIN || n > INT32_MAX)
			{
				return false;
			}
			out = static_cast<std::int32_t>(n);
			return true;
		}

		bool String(const json& j, const char* key, std::string& out)
		{
			auto it = j.find(key);
			if (it == j.end() || !it->is_string())
			{
				return false;
			}
			out = it->get<std::string>();
			return true;
		}

		bool Boolean(const json& j, const char* key, bool& out)
		{
			auto it = j.find(key);
			if (it == j.end() || !it->is_boolean())
			{
				return false;
			}
			out = it->get<bool>();
			return true;
		}

		bool Floats(const json& j, float* out, std::size_t n)
		{
			if (!j.is_array() || j.size() != n)
			{
				return false;
			}
			for (std::size_t i = 0; i < n; ++i)
			{
				if (!j[i].is_number())
				{
					return false;
				}
				out[i] = j[i].get<float>();
				if (!std::isfinite(out[i]))
				{
					return false;
				}
			}
			return true;
		}

		bool Vec(const json& j, const char* key, float* out, std::size_t n)
		{
			auto it = j.find(key);
			return it != j.end() && Floats(*it, out, n);
		}

		bool Array(const json& j, const char* key)
		{
			return j.contains(key) && j[key].is_array();
		}

		std::uint32_t U32(const std::byte* p)
		{
			std::uint32_t n;
			std::memcpy(&n, p, 4);
			return n;
		}
	} // namespace

	Expected<GltfAsset> GltfAsset::FromPrepared(std::span<const std::byte> data, std::string_view meshPath)
	{
		auto fail = [&]() -> Expected<GltfAsset>
		{
			return std::unexpected(AetherError::Asset("Invalid prepared browser model: " + std::string(meshPath)));
		};
		if (data.size() < 16 || std::memcmp(data.data(), "AEP2", 4) != 0)
		{
			return fail();
		}
		const std::uint32_t metadata = U32(data.data() + 4), streams = U32(data.data() + 8);
		if (metadata > data.size() - 16 || streams > data.size() - 16 - metadata)
		{
			return fail();
		}
		auto doc = json::parse(reinterpret_cast<const char*>(data.data() + 16), reinterpret_cast<const char*>(data.data() + 16 + metadata), nullptr, false);
		if (doc.is_discarded() || !doc.is_object() || !Array(doc, "materials") || !Array(doc, "nodes") || !Array(doc, "skins") || !Array(doc, "animations") || !Array(doc, "primitives") || !Array(doc, "bounds"))
		{
			return fail();
		}
		if (doc["materials"].size() > 65536 || doc["nodes"].size() > 4097 || doc["skins"].size() > 1 || doc["animations"].size() > 65536 || doc["primitives"].size() > 65536)
		{
			return fail();
		}
		float bounds[10];
		if (!Floats(doc["bounds"], bounds, 10))
		{
			return fail();
		}
		GltfAsset result;
		for (const auto& j: doc["materials"])
		{
			if (!j.is_object())
			{
				return fail();
			}
			GltfMaterial m;
			if (!String(j, "name", m.name) || !Vec(j, "baseColorFactor", &m.baseColorFactor[0], 4) || !Number(j, "metallicFactor", m.metallicFactor) || !Number(j, "roughnessFactor", m.roughnessFactor)
			        || !Vec(j, "emissiveFactor", &m.emissiveFactor[0], 3) || !Number(j, "alphaCutoff", m.alphaCutoff) || !Vec(j, "uvScroll", &m.uvScroll[0], 2))
			{
				return fail();
			}
#define AE_PREP_BOOL(field) if(!Boolean(j,#field,m.field)) return fail()
			AE_PREP_BOOL(doubleSided);
			AE_PREP_BOOL(alphaBlend);
			AE_PREP_BOOL(additiveBlend);
			AE_PREP_BOOL(alphaMask);
			AE_PREP_BOOL(modulateVertexColor);
			AE_PREP_BOOL(foliage);
			AE_PREP_BOOL(bakedLighting);
			AE_PREP_BOOL(sky);
			AE_PREP_BOOL(lightShaft);
			AE_PREP_BOOL(gem);
			AE_PREP_BOOL(sheen);
			AE_PREP_BOOL(water);
			AE_PREP_BOOL(character);
			AE_PREP_BOOL(objectLit);
#undef AE_PREP_BOOL
#define AE_PREP_STRING(field) if(!String(j,#field,m.field)) return fail()
			AE_PREP_STRING(albedoPath);
			AE_PREP_STRING(normalPath);
			AE_PREP_STRING(metallicRoughnessPath);
			AE_PREP_STRING(occlusionPath);
			AE_PREP_STRING(emissivePath);
			AE_PREP_STRING(shaderVfsPath);
#undef AE_PREP_STRING
			result.materials.push_back(std::move(m));
		}
		for (const auto& j: doc["nodes"])
		{
			if (!j.is_object())
			{
				return fail();
			}
			GltfNode n;
			float rotation[4];
			if (!String(j, "name", n.name) || !Integer(j, "parentIndex", n.parentIndex) || !Integer(j, "meshIndex", n.meshIndex) || !Integer(j, "skinIndex", n.skinIndex) || !Vec(j, "translation", &n.translation[0], 3)
			        || !Vec(j, "rotation", rotation, 4) || !Vec(j, "scale", &n.scale[0], 3) || !Vec(j, "matrix", &n.matrix[0][0], 16) || !Boolean(j, "hasMatrix", n.hasMatrix) || !Array(j, "children"))
			{
				return fail();
			}
			n.rotation = glm::quat(rotation[3], rotation[0], rotation[1], rotation[2]);
			for (const auto& v: j["children"])
			{
				if (!v.is_number_unsigned())
				{
					return fail();
				}
				auto index = v.get<std::uint64_t>();
				if (index >= doc["nodes"].size())
				{
					return fail();
				}
				n.children.push_back(static_cast<std::uint32_t>(index));
			}
			result.nodes.push_back(std::move(n));
		}
		for (const auto& j: doc["skins"])
		{
			if (!j.is_object())
			{
				return fail();
			}
			GltfSkin s;
			if (!String(j, "name", s.name) || !Integer(j, "skeletonRoot", s.skeletonRoot) || !Array(j, "joints") || !Array(j, "inverseBindMatrices") || j["joints"].size() != j["inverseBindMatrices"].size() || j["joints"].size() > 4096)
			{
				return fail();
			}
			for (std::size_t i = 0; i < j["joints"].size(); ++i)
			{
				if (!j["joints"][i].is_number_unsigned())
				{
					return fail();
				}
				auto index = j["joints"][i].get<std::uint64_t>();
				if (index >= result.nodes.size())
				{
					return fail();
				}
				glm::mat4 matrix;
				if (!Floats(j["inverseBindMatrices"][i], &matrix[0][0], 16))
				{
					return fail();
				}
				s.joints.push_back(static_cast<std::uint32_t>(index));
				s.inverseBindMatrices.push_back(matrix);
			}
			result.skins.push_back(std::move(s));
		}
		for (const auto& j: doc["animations"])
		{
			if (!j.is_object())
			{
				return fail();
			}
			GltfAnimation a;
			if (!String(j, "name", a.name) || !Boolean(j, "rootLocked", a.rootLocked) || !Array(j, "channels") || j["channels"].size() > 65536)
			{
				return fail();
			}
			for (const auto& c: j["channels"])
			{
				if (!c.is_object())
				{
					return fail();
				}
				GltfAnimationChannel channel;
				std::int32_t node, path, interpolation;
				if (!Integer(c, "nodeIndex", node) || node < 0 || !Integer(c, "path", path) || path < 0 || path > 3 || !Integer(c, "interpolation", interpolation) || interpolation < 0 || interpolation > 2 || !String(c, "boneName", channel.boneName)
				        || !Array(c, "times") || !Array(c, "values") || c["times"].size() > (1u << 20) || c["values"].size() != c["times"].size() * (interpolation == 2 ? 3 : 1))
				{
					return fail();
				}
				channel.nodeIndex = static_cast<std::uint32_t>(node);
				channel.path = static_cast<GltfAnimationPath>(path);
				channel.interpolation = static_cast<GltfInterpolation>(interpolation);
				for (const auto& t: c["times"])
				{
					if (!t.is_number())
					{
						return fail();
					}
					auto value = t.get<float>();
					if (!std::isfinite(value))
					{
						return fail();
					}
					channel.times.push_back(value);
				}
				for (const auto& v: c["values"])
				{
					glm::vec4 value;
					if (!Floats(v, &value[0], 4))
					{
						return fail();
					}
					channel.values.push_back(value);
				}
				a.channels.push_back(std::move(channel));
			}
			result.animations.push_back(std::move(a));
		}
		std::size_t cursor = 16 + metadata, end = cursor + streams;
		for (const auto& j: doc["primitives"])
		{
			if (!j.is_object())
			{
				return fail();
			}
			GltfPrimitive primitive;
			std::int32_t vertexCount, indexCount;
			if (!Integer(j, "materialIndex", primitive.materialIndex) || !Integer(j, "skinIndex", primitive.skinIndex) || !String(j, "primitiveKey", primitive.primitiveKey) || !Integer(j, "vertexCount", vertexCount)
			        || !Integer(j, "indexCount", indexCount) || vertexCount < 0 || indexCount < 0)
			{
				return fail();
			}
			const auto vertexBytes = static_cast<std::uint64_t>(vertexCount) * sizeof(MeshVertex), indexBytes = static_cast<std::uint64_t>(indexCount) * sizeof(std::uint32_t);
			if (vertexBytes > end - cursor || indexBytes > end - cursor - vertexBytes)
			{
				return fail();
			}
			primitive.vertices.resize(vertexCount);
			primitive.indices.resize(indexCount);
			std::memcpy(primitive.vertices.data(), data.data() + cursor, static_cast<std::size_t>(vertexBytes));
			cursor += static_cast<std::size_t>(vertexBytes);
			std::memcpy(primitive.indices.data(), data.data() + cursor, static_cast<std::size_t>(indexBytes));
			cursor += static_cast<std::size_t>(indexBytes);
			std::memcpy(primitive.aabbMin, bounds, 3 * sizeof(float));
			std::memcpy(primitive.aabbMax, bounds + 3, 3 * sizeof(float));
			std::memcpy(primitive.sphereCenter, bounds + 6, 3 * sizeof(float));
			primitive.sphereRadius = bounds[9];
			result.primitives.push_back(std::move(primitive));
		}
		if (cursor != end || result.nodes.empty() || result.primitives.empty())
		{
			return fail();
		}
		return result;
	}
} // namespace aether::assets
