#include "MaterialProcessor.hpp"

#include <BinaryFormats.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace aether::assetpipeline
{
	namespace MaterialProcessor
	{
		ByteBuffer Process(std::span<const std::byte> tomlData, const std::filesystem::path& sourcePath, const std::filesystem::path& /*sourceDir*/)
		{
			const std::string_view text(reinterpret_cast<const char*>(tomlData.data()), tomlData.size());

			toml::table tbl;
			try
			{
				tbl = toml::parse(text);
			}
			catch (const toml::parse_error& e)
			{
				std::cerr << "  MaterialProcessor: TOML parse error in " << sourcePath << ": " << e.description() << "\n";
				return {};
			}

			// TOML integers are idiomatic for whole values (e.g. roughnessFactor = 0); accept both node kinds.
			auto readNumber = [](const toml::node* node, float& out) -> bool
			{
				if (node == nullptr)
				{
					return false;
				}
				if (auto* const f = node->as_floating_point())
				{
					out = static_cast<float>(f->get());
					return true;
				}
				if (auto* const i = node->as_integer())
				{
					out = static_cast<float>(i->get());
					return true;
				}
				return false;
			};

			auto readFloats = [&](const char* section, const char* key, float* out, int count, const std::vector<double>& fallback)
			{
				auto* const arr = tbl[section][key].as_array();
				if (!arr)
				{
					for (int i = 0; i < count && i < static_cast<int>(fallback.size()); ++i)
					{
						out[i] = static_cast<float>(fallback[i]);
					}
					return;
				}
				for (int i = 0; i < count && i < static_cast<int>(arr->size()); ++i)
				{
					float value = 0.f;
					if (readNumber(arr->get(i), value))
					{
						out[i] = value;
					}
				}
			};

			float baseColor[4] = {1, 1, 1, 1};
			float metallic = 1;
			float roughness = 1;
			float emissive[3] = {0, 0, 0};
			float alphaCutoff = 0;

			readFloats("material", "baseColorFactor", baseColor, 4, {1.0, 1.0, 1.0, 1.0});
			readFloats("material", "emissiveFactor", emissive, 3, {0.0, 0.0, 0.0});

			{
				float value = 0.f;
				if (readNumber(tbl["material"]["metallicFactor"].node(), value))
				{
					metallic = value;
				}
			}
			{
				float value = 0.f;
				if (readNumber(tbl["material"]["roughnessFactor"].node(), value))
				{
					roughness = value;
				}
			}
			{
				float value = 0.f;
				if (readNumber(tbl["material"]["alphaCutoff"].node(), value))
				{
					alphaCutoff = value;
				}
			}

			const bool doubleSided = [&]() -> bool
			{
				auto* const v = tbl["material"]["doubleSided"].as_boolean();
				return v && v->get();
			}();
			const bool alphaBlend = [&]() -> bool
			{
				auto* const v = tbl["material"]["alphaBlend"].as_boolean();
				return v && v->get();
			}();
			const bool alphaMask = [&]() -> bool
			{
				auto* const v = tbl["material"]["alphaMask"].as_boolean();
				return v && v->get();
			}();
			const bool modulateVertexColor = [&]() -> bool
			{
				auto* const v = tbl["material"]["modulateVertexColor"].as_boolean();
				return v && v->get();
			}();

			std::string shaderVfsPath;
			{
				auto* const v = tbl["material"]["shader"].as_string();
				if (v)
				{
					shaderVfsPath = v->get();
				}
			}

			struct TexEntry
			{
				uint8_t type;
				std::string path;
			};

			std::vector<TexEntry> textures;

			auto addTex = [&](const char* key, TextureTypeDisk type)
			{
				auto* const v = tbl["textures"][key].as_string();
				if (v)
				{
					textures.push_back({static_cast<uint8_t>(type), std::string(v->get())});
				}
			};

			addTex("albedo", TextureTypeDisk::BaseColor);
			addTex("normal", TextureTypeDisk::Normal);
			addTex("metallicRoughness", TextureTypeDisk::MetallicRoughness);
			addTex("occlusion", TextureTypeDisk::Occlusion);
			addTex("emissive", TextureTypeDisk::Emissive);

			MaterialHeaderDisk hdr;
			std::memcpy(hdr.baseColorFactor, baseColor, sizeof(baseColor));
			hdr.metallicFactor = metallic;
			hdr.roughnessFactor = roughness;
			std::memcpy(hdr.emissiveFactor, emissive, sizeof(emissive));
			hdr.alphaCutoff = alphaCutoff;
			hdr.doubleSided = doubleSided ? 1 : 0;
			hdr.alphaBlend = alphaBlend ? 1 : 0;
			hdr.alphaMask = alphaMask ? 1 : 0;
			hdr.modulateVertexColor = modulateVertexColor ? 1 : 0;
			hdr.texturePathCount = static_cast<uint8_t>(textures.size());

			std::vector<std::byte> out;
			auto append = [&](const void* data, std::size_t n)
			{
				const auto* const p = reinterpret_cast<const std::byte*>(data);
				out.insert(out.end(), p, p + n);
			};

			append(&hdr, sizeof(hdr));

			for (const auto& tex: textures)
			{
				const uint16_t pathLen = static_cast<uint16_t>(tex.path.size());
				append(&tex.type, sizeof(tex.type));
				append(&pathLen, sizeof(pathLen));
				append(tex.path.data(), tex.path.size());
			}

			{
				const uint16_t shaderPathLen = static_cast<uint16_t>(shaderVfsPath.size());
				append(&shaderPathLen, sizeof(shaderPathLen));
				if (shaderPathLen > 0)
				{
					append(shaderVfsPath.data(), shaderVfsPath.size());
				}
			}

			return out;
		}
	} // namespace MaterialProcessor
} // namespace aether::assetpipeline
