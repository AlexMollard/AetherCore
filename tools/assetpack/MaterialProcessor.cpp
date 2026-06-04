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

namespace MaterialProcessor
{
	ByteBuffer Process(
	    std::span<const std::byte>    tomlData,
	    const std::filesystem::path&  sourcePath,
	    const std::filesystem::path&  sourceDir)
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

		auto readFloats = [&](const char* section, const char* key, float* out, int count, const std::vector<double>& fallback)
		{
			const auto* arr = tbl[section][key].as_array();
			if (!arr)
			{
				for (int i = 0; i < count && i < static_cast<int>(fallback.size()); ++i)
					out[i] = static_cast<float>(fallback[i]);
				return;
			}
			for (int i = 0; i < count && i < static_cast<int>(arr->size()); ++i)
			{
				const auto* v = arr->get(i)->as_floating_point();
				if (v)
					out[i] = static_cast<float>(v->get());
			}
		};

		float baseColor[4] = { 1, 1, 1, 1 };
		float metallic = 1;
		float roughness = 1;
		float emissive[3] = { 0, 0, 0 };
		float alphaCutoff = 0;

		readFloats("material", "baseColorFactor", baseColor, 4, { 1.0, 1.0, 1.0, 1.0 });
		readFloats("material", "emissiveFactor",  emissive, 3, { 0.0, 0.0, 0.0 });

		{
			const auto* v = tbl["material"]["metallicFactor"].as_floating_point();
			if (v) metallic = static_cast<float>(v->get());
		}
		{
			const auto* v = tbl["material"]["roughnessFactor"].as_floating_point();
			if (v) roughness = static_cast<float>(v->get());
		}
		{
			const auto* v = tbl["material"]["alphaCutoff"].as_floating_point();
			if (v) alphaCutoff = static_cast<float>(v->get());
		}

		const bool doubleSided = [&]() -> bool {
			const auto* v = tbl["material"]["doubleSided"].as_boolean();
			return v && v->get();
		}();
		const bool alphaBlend = [&]() -> bool {
			const auto* v = tbl["material"]["alphaBlend"].as_boolean();
			return v && v->get();
		}();
		const bool alphaMask = [&]() -> bool {
			const auto* v = tbl["material"]["alphaMask"].as_boolean();
			return v && v->get();
		}();

		struct TexEntry
		{
			uint8_t type;
			std::string path;
		};
		std::vector<TexEntry> textures;

		auto addTex = [&](const char* key, TextureTypeDisk type)
		{
			const auto* v = tbl["textures"][key].as_string();
			if (v)
				textures.push_back({ static_cast<uint8_t>(type), std::string(v->get()) });
		};

		addTex("albedo",            TextureTypeDisk::BaseColor);
		addTex("normal",            TextureTypeDisk::Normal);
		addTex("metallicRoughness", TextureTypeDisk::MetallicRoughness);
		addTex("occlusion",         TextureTypeDisk::Occlusion);
		addTex("emissive",          TextureTypeDisk::Emissive);

		MaterialHeaderDisk hdr;
		std::memcpy(hdr.baseColorFactor, baseColor, sizeof(baseColor));
		hdr.metallicFactor = metallic;
		hdr.roughnessFactor = roughness;
		std::memcpy(hdr.emissiveFactor, emissive, sizeof(emissive));
		hdr.alphaCutoff = alphaCutoff;
		hdr.doubleSided = doubleSided ? 1 : 0;
		hdr.alphaBlend = alphaBlend ? 1 : 0;
		hdr.alphaMask = alphaMask ? 1 : 0;
		hdr.texturePathCount = static_cast<uint8_t>(textures.size());

		std::vector<std::byte> out;
		auto append = [&](const void* data, std::size_t n)
		{
			const auto* p = reinterpret_cast<const std::byte*>(data);
			out.insert(out.end(), p, p + n);
		};

		append(&hdr, sizeof(hdr));

		for (const auto& tex : textures)
		{
			const uint16_t pathLen = static_cast<uint16_t>(tex.path.size());
			append(&tex.type, sizeof(tex.type));
			append(&pathLen, sizeof(pathLen));
			append(tex.path.data(), tex.path.size());
		}

		return out;
	}
} // namespace MaterialProcessor
