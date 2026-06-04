#include "MaterialProcessor.hpp"

#include <BinaryFormats.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace MaterialProcessor
{
	static std::string Trim(std::string s)
	{
		const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r'; };
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
		s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
		return s;
	}

	static float ParseFloat(const std::string& s)
	{
		char* end = nullptr;
		const double v = std::strtod(s.c_str(), &end);
		return static_cast<float>(v);
	}

	static bool ParseBool(const std::string& s)
	{
		const std::string lower = [&]() {
			std::string r;
			r.reserve(s.size());
			for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return r;
		}();
		return lower == "true" || lower == "yes";
	}

	// Parse a TOML array like [1.0, 2.0, 3.0, 4.0] into floats
	static void ParseFloatArray(const std::string& val, float* out, int count)
	{
		std::string inner;
		auto start = val.find('[');
		auto end = val.rfind(']');
		if (start != std::string::npos && end != std::string::npos && end > start)
			inner = val.substr(start + 1, end - start - 1);
		else
			inner = val;

		std::istringstream ss(inner);
		std::string token;
		int idx = 0;
		while (std::getline(ss, token, ',') && idx < count)
			out[idx++] = ParseFloat(Trim(token));
	}

	static std::string ParseString(const std::string& val)
	{
		auto s = Trim(val);
		if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
			return s.substr(1, s.size() - 2);
		return s;
	}

	// Simple TOML section-key parser for material configs.
	// Handles [section] headers and key = value lines (arrays, floats, bools, strings).
	static void ParseTomlSimple(const std::string& text,
	    const std::function<void(const std::string& section, const std::string& key, const std::string& value)>& callback)
	{
		std::string section;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			line = Trim(line);
			if (line.empty() || line[0] == '#')
				continue;

			if (line[0] == '[')
			{
				auto end = line.find(']');
				if (end != std::string::npos)
					section = Trim(line.substr(1, end - 1));
				continue;
			}

			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;

			std::string key = Trim(line.substr(0, eq));
			std::string val = Trim(line.substr(eq + 1));
			if (!key.empty())
				callback(section, key, val);
		}
	}

	std::vector<std::byte> Process(
	    const std::vector<std::byte>& tomlData,
	    const std::filesystem::path&  sourcePath,
	    const std::filesystem::path&  sourceDir)
	{
		// Convert to string for parsing
		std::string text(reinterpret_cast<const char*>(tomlData.data()), tomlData.size());

		float baseColor[4] = { 1, 1, 1, 1 };
		float metallic = 1;
		float roughness = 1;
		float emissive[3] = { 0, 0, 0 };
		float alphaCutoff = 0;
		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;

		struct TexEntry
		{
			uint8_t type;
			std::string path;
		};
		std::vector<TexEntry> textures;

		ParseTomlSimple(text, [&](const std::string& section, const std::string& key, const std::string& value)
		{
			if (section == "material")
			{
				if (key == "baseColorFactor")
					ParseFloatArray(value, baseColor, 4);
				else if (key == "emissiveFactor")
					ParseFloatArray(value, emissive, 3);
				else if (key == "metallicFactor")
					metallic = ParseFloat(value);
				else if (key == "roughnessFactor")
					roughness = ParseFloat(value);
				else if (key == "alphaCutoff")
					alphaCutoff = ParseFloat(value);
				else if (key == "doubleSided")
					doubleSided = ParseBool(value);
				else if (key == "alphaBlend")
					alphaBlend = ParseBool(value);
				else if (key == "alphaMask")
					alphaMask = ParseBool(value);
			}
			else if (section == "textures")
			{
				auto addTex = [&](const std::string& texKey, TextureTypeDisk type)
				{
					if (key == texKey)
						textures.push_back({ static_cast<uint8_t>(type), ParseString(value) });
				};
				addTex("albedo",            TextureTypeDisk::BaseColor);
				addTex("normal",            TextureTypeDisk::Normal);
				addTex("metallicRoughness", TextureTypeDisk::MetallicRoughness);
				addTex("occlusion",         TextureTypeDisk::Occlusion);
				addTex("emissive",          TextureTypeDisk::Emissive);
			}
		});

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
