#include "material/MaterialSerializer.hpp"

#include <format>
#include <filesystem>

#include <BinaryFormats.hpp>

#include "utils/BinaryReader.hpp"

#include "io/FileSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	namespace MaterialSerializer
	{
	namespace
	{
		std::string NormalizeVirtualFolder(std::string path)
		{
			while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
			{
				path.pop_back();
			}
			return path;
		}

		std::string ResolvePathInFolder(std::string_view folderPath, const std::string& resourcePath)
		{
			if (resourcePath.empty())
			{
				return {};
			}
			if (resourcePath.contains("://"))
			{
				return resourcePath;
			}
			const std::string folder = NormalizeVirtualFolder(std::string(folderPath));
			return folder + "/" + resourcePath;
		}

		std::string ResolveStemInFolder(std::string_view folderPath, const std::string& stem)
		{
			if (stem.empty())
			{
				return {};
			}

			if (stem.contains("://"))
			{
				return io::FileSystem::Exists(stem) ? stem : std::string();
			}

			std::string direct = ResolvePathInFolder(folderPath, stem);
			if (io::FileSystem::Exists(direct))
			{
				return direct;
			}

			const std::string noExt = std::filesystem::path(stem).extension().empty() ? stem : std::filesystem::path(stem).stem().string();
			constexpr std::string_view kExts[] = {".texture", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".webp", ".dds", ".ktx2"};
			for (const std::string_view ext: kExts)
			{
				const std::string candidate = ResolvePathInFolder(folderPath, noExt + std::string(ext));
				if (io::FileSystem::Exists(candidate))
				{
					return candidate;
				}
			}

			return {};
		}

		std::string ResolveFirstAliasInFolder(std::string_view folderPath, std::initializer_list<std::string_view> aliases)
		{
			for (const std::string_view alias: aliases)
			{
				const std::string resolved = ResolveStemInFolder(folderPath, std::string(alias));
				if (!resolved.empty())
				{
					return resolved;
				}
			}
			return {};
		}

		struct BooleanField
		{
			std::string_view key;
			bool MaterialAsset::* member;
		};
		constexpr BooleanField kAdditionalFlags[] = {
		        {"additiveblend", &MaterialAsset::additiveBlend}, {"foliage", &MaterialAsset::foliage},
		        {"bakedlighting", &MaterialAsset::bakedLighting}, {"sky", &MaterialAsset::sky},
		        {"lightshaft", &MaterialAsset::lightShaft}, {"objectlit", &MaterialAsset::objectLit},
		        {"gem", &MaterialAsset::gem}, {"sheen", &MaterialAsset::sheen},
		        {"water", &MaterialAsset::water}, {"character", &MaterialAsset::character}};

		bool IsSafePath(std::string_view path)
		{
			if (path.find('\0') != std::string_view::npos)
			{
				return false;
			}
			const auto mount = path.find("://");
			const auto relative = mount == std::string_view::npos ? path : path.substr(mount + 3);
			if (relative.starts_with('/') || relative.starts_with('\\') || relative.contains(':'))
			{
				return false;
			}
			std::size_t at = 0;
			while (at < relative.size())
			{
				const auto end = relative.find_first_of("/\\", at);
				if (relative.substr(at, end == std::string_view::npos ? relative.size() - at : end - at) == "..")
				{
					return false;
				}
				if (end == std::string_view::npos) { break; }
				at = end + 1;
			}
			return true;
		}

		bool ReadCheckedString(BinaryReader& reader, std::string& value)
		{
			std::uint16_t size = 0;
			if (!reader.TryRead(size) || !reader.CanRead(size))
			{
				return false;
			}
			value.assign(reinterpret_cast<const char*>(reader.Data()), size);
			reader.Advance(size);
			return true;
		}

		Expected<MaterialPresetSpec> LoadBinary(std::string_view path)
		{
			AE_TRY(bytes, io::FileSystem::ReadFile(path));
			if (bytes->size() < sizeof(MaterialHeaderDisk))
			{
				AE_UNEXPECTED(AetherError::Asset("Truncated material header: " + std::string(path)));
			}
			BinaryReader reader(*bytes);
			MaterialHeaderDisk hdr;
			if (!reader.TryRead(hdr) || !CheckMagic(hdr) || hdr.version < 1 || hdr.version > MATL_VERSION)
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid material header: " + std::string(path)));
			}
			MaterialPresetSpec spec;
			auto& material = spec.material;
			material.baseColorFactor = glm::vec4(hdr.baseColorFactor[0], hdr.baseColorFactor[1], hdr.baseColorFactor[2], hdr.baseColorFactor[3]);
			material.metallicFactor = hdr.metallicFactor;
			material.roughnessFactor = hdr.roughnessFactor;
			material.emissiveFactor = glm::vec3(hdr.emissiveFactor[0], hdr.emissiveFactor[1], hdr.emissiveFactor[2]);
			material.alphaCutoff = hdr.alphaCutoff;
			material.doubleSided = hdr.doubleSided != 0;
			material.alphaBlend = hdr.alphaBlend != 0;
			material.additiveBlend = hdr.alphaBlend == 2;
			material.alphaMask = hdr.alphaMask != 0;
			material.modulateVertexColor = hdr.modulateVertexColor != 0;
			material.foliage = hdr.foliage != 0;
			material.bakedLighting = (hdr.bakedLighting & kMaterialDiskBaked) != 0;
			material.sky = (hdr.bakedLighting & kMaterialDiskSky) != 0;
			material.lightShaft = (hdr.bakedLighting & kMaterialDiskLightShaft) != 0;
			material.gem = (hdr.bakedLighting & kMaterialDiskGem) != 0;
			material.sheen = (hdr.bakedLighting & kMaterialDiskSheen) != 0;
			material.water = (hdr.bakedLighting & kMaterialDiskWater) != 0;
			material.character = (hdr.bakedLighting & kMaterialDiskCharacter) != 0;
			material.objectLit = hdr.objectLit != 0;
			material.uvScroll = glm::vec2(hdr.uvScroll[0], hdr.uvScroll[1]);

			for (std::uint8_t t = 0; t < hdr.texturePathCount; ++t)
			{
				std::uint8_t type = 0;
				std::string reference;
				if (!reader.TryRead(type) || !ReadCheckedString(reader, reference) || reference.empty())
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid material texture record: " + std::string(path)));
				}
				const auto resolved = io::FileSystem::ResolveRelative(path, reference);
				if (!IsSafePath(resolved))
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid material texture path: " + std::string(path)));
				}
				switch (static_cast<TextureTypeDisk>(type))
				{
					case TextureTypeDisk::BaseColor: spec.albedoPath = resolved; break;
					case TextureTypeDisk::Normal: spec.normalPath = resolved; break;
					case TextureTypeDisk::MetallicRoughness: spec.metallicRoughnessPath = resolved; break;
					case TextureTypeDisk::Occlusion: spec.occlusionPath = resolved; break;
					case TextureTypeDisk::Emissive: spec.emissivePath = resolved; break;
					default: AE_UNEXPECTED(AetherError::Asset("Unknown material texture slot: " + std::string(path)));
				}
			}
			// Original cooked materials predate the optional shader string trailer.
			if (reader.Remaining() != 0 && !ReadCheckedString(reader, spec.shaderVfsPath))
			{
				AE_UNEXPECTED(AetherError::Asset("Truncated material shader path: " + std::string(path)));
			}
			if (reader.Remaining() != 0 || !IsSafePath(spec.shaderVfsPath))
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid material shader path or trailing data: " + std::string(path)));
			}
			return spec;
		}

		// True for "[graph]" and "[graph.node.0]" but not "[graphics]".
		bool IsGraphHeader(std::string_view trimmed)
		{
			return trimmed.starts_with("[graph]") || trimmed.starts_with("[graph.");
		}

		std::string_view TrimLeft(std::string_view line)
		{
			const std::size_t at = line.find_first_not_of(" \t");
			return at == std::string_view::npos ? std::string_view{} : line.substr(at);
		}

		// Lifts the graph tables out of a material file as raw text, so writing the material
		// back preserves a graph this code does not understand. Reconstructing it from parsed
		// key-value pairs would silently drop anything a newer editor wrote.
		std::string ExtractGraphSection(const std::string& text)
		{
			std::string out;
			bool inGraph = false;
			std::size_t at = 0;
			while (at <= text.size())
			{
				const std::size_t eol = text.find('\n', at);
				const std::size_t end = (eol == std::string::npos) ? text.size() : eol;
				const std::string_view line(text.data() + at, end - at);
				const std::string_view trimmed = TrimLeft(line);

				if (trimmed.starts_with('['))
				{
					inGraph = IsGraphHeader(trimmed);
				}
				if (inGraph)
				{
					out.append(line);
					out.push_back('\n');
				}
				if (eol == std::string::npos)
				{
					break;
				}
				at = eol + 1;
			}
			// A trailing blank line would grow by one on every round trip.
			while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
			{
				out.pop_back();
			}
			return out;
		}
	} // namespace

	Expected<MaterialPresetSpec> Load(std::string_view path)
	{
		if (path.empty() || !IsSafePath(path))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid material path: " + std::string(path)));
		}
		const auto requested = NormalizeVirtualFolder(std::string(path));
		std::string presetPath = requested;
		std::string folderPath;
		if (io::FileSystem::Exists(requested + "/properties.toml"))
		{
			folderPath = requested;
			presetPath += "/properties.toml";
		}
		else if (io::FileSystem::Exists(requested + ".material"))
		{
			// Publishing replaces folder/properties.toml with folder.material while
			// retaining the folder's automatically named texture maps beside it.
			folderPath = requested;
			presetPath += ".material";
		}
		MaterialPresetSpec spec;
		if (presetPath.ends_with(".material"))
		{
			AE_TRY(loaded, LoadBinary(presetPath));
			spec = std::move(*loaded);
		}
		else
		{
			AE_TRY(text, io::FileSystem::ReadFileText(presetPath));
			bool ok = false;
			spec = Parse(presetPath, *text, &ok);
			if (!ok)
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid material TOML: " + presetPath));
			}
		}
		if (!folderPath.empty())
		{
			if (spec.albedoPath.empty()) { spec.albedoPath = ResolveFirstAliasInFolder(folderPath, {"albedo", "basecolor", "base_color", "diffuse", "color"}); }
			if (spec.normalPath.empty()) { spec.normalPath = ResolveFirstAliasInFolder(folderPath, {"normal", "nrm"}); }
			if (spec.metallicRoughnessPath.empty()) { spec.metallicRoughnessPath = ResolveFirstAliasInFolder(folderPath, {"metallicroughness", "metal_rough", "metalrough", "orm", "roughness", "metallic"}); }
			if (spec.occlusionPath.empty()) { spec.occlusionPath = ResolveFirstAliasInFolder(folderPath, {"occlusion", "ao", "ambientocclusion"}); }
			if (spec.emissivePath.empty()) { spec.emissivePath = ResolveFirstAliasInFolder(folderPath, {"emissive", "emission"}); }
		}
		for (const auto* reference: {&spec.albedoPath, &spec.normalPath, &spec.metallicRoughnessPath, &spec.occlusionPath, &spec.emissivePath, &spec.shaderVfsPath})
		{
			if (!IsSafePath(*reference))
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid material resource path: " + presetPath));
			}
		}
		return spec;
	}

	MaterialPresetSpec Parse(std::string_view path, const std::string& text, bool* ok)
	{
		MaterialPresetSpec spec;
		spec.graphSection = ExtractGraphSection(text);
		bool versionSupported = true;

		const bool parsed = text::ParseToml(text,
		        [&spec, &versionSupported, path](const text::IniEntry& entry)
		        {
			        if (entry.fullKey == "material.schema_version" || entry.fullKey == "schema_version")
			        {
				        versionSupported = versionSupported && entry.tomlValue == "1";
				        return;
			        }
			        for (const auto& field: kAdditionalFlags)
			        {
				        if (entry.fullKey == field.key || entry.fullKey == std::string("material.") + std::string(field.key))
				        {
					        if (const auto parsed = text::ParseBool(entry.value)) { spec.material.*field.member = *parsed; }
					        return;
				        }
			        }
			        if (entry.fullKey == "material.basecolorfactor" || entry.fullKey == "basecolorfactor")
			        {
				        if (const auto parsed = text::ParseFloatArray<4>(entry.value))
				        {
					        spec.material.baseColorFactor = glm::vec4((*parsed)[0], (*parsed)[1], (*parsed)[2], (*parsed)[3]);
				        }
				        return;
			        }
			        if (entry.fullKey == "material.emissivefactor" || entry.fullKey == "emissivefactor")
			        {
				        if (const auto parsed = text::ParseFloatArray<3>(entry.value))
				        {
					        spec.material.emissiveFactor = glm::vec3((*parsed)[0], (*parsed)[1], (*parsed)[2]);
				        }
				        return;
			        }
			        if (entry.fullKey == "material.metallicfactor" || entry.fullKey == "metallicfactor")
			        {
				        if (const auto parsed = text::ParseFloat(entry.value))
				        {
					        spec.material.metallicFactor = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.roughnessfactor" || entry.fullKey == "roughnessfactor")
			        {
				        if (const auto parsed = text::ParseFloat(entry.value))
				        {
					        spec.material.roughnessFactor = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.occlusionstrength" || entry.fullKey == "occlusionstrength")
			        {
				        if (const auto parsed = text::ParseFloat(entry.value))
				        {
					        spec.material.occlusionStrength = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.alphacutoff" || entry.fullKey == "alphacutoff")
			        {
				        if (const auto parsed = text::ParseFloat(entry.value))
				        {
					        spec.material.alphaCutoff = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.uvscroll" || entry.fullKey == "uvscroll")
			        {
				        if (const auto parsed = text::ParseFloatArray<2>(entry.value))
				        {
					        spec.material.uvScroll = glm::vec2((*parsed)[0], (*parsed)[1]);
				        }
				        return;
			        }
			        if (entry.fullKey == "material.distancefade" || entry.fullKey == "distancefade")
			        {
				        if (const auto parsed = text::ParseFloatArray<2>(entry.value))
				        {
					        spec.material.distanceFade = glm::vec2((*parsed)[0], (*parsed)[1]);
				        }
				        return;
			        }
			        if (entry.fullKey == "material.doublesided" || entry.fullKey == "doublesided")
			        {
				        if (const auto parsed = text::ParseBool(entry.value))
				        {
					        spec.material.doubleSided = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.alphablend" || entry.fullKey == "alphablend")
			        {
				        if (const auto parsed = text::ParseBool(entry.value))
				        {
					        spec.material.alphaBlend = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.alphamask" || entry.fullKey == "alphamask")
			        {
				        if (const auto parsed = text::ParseBool(entry.value))
				        {
					        spec.material.alphaMask = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.modulatevertexcolor" || entry.fullKey == "modulatevertexcolor")
			        {
				        if (const auto parsed = text::ParseBool(entry.value))
				        {
					        spec.material.modulateVertexColor = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.receiveshadows" || entry.fullKey == "receiveshadows")
			        {
				        if (const auto parsed = text::ParseBool(entry.value))
				        {
					        spec.material.receiveShadows = *parsed;
				        }
				        return;
			        }
			        if (entry.fullKey == "material.shader" || entry.fullKey == "shader")
			        {
				        spec.shaderVfsPath = entry.value;
				        return;
			        }

			        if (entry.fullKey == "textures.albedo" || entry.fullKey == "albedo" || entry.fullKey == "textures.basecolor")
			        {
				        spec.albedoPath = io::FileSystem::ResolveRelative(path, entry.value);
				        return;
			        }
			        if (entry.fullKey == "textures.normal" || entry.fullKey == "normal")
			        {
				        spec.normalPath = io::FileSystem::ResolveRelative(path, entry.value);
				        return;
			        }
			        if (entry.fullKey == "textures.metallicroughness" || entry.fullKey == "metallicroughness")
			        {
				        spec.metallicRoughnessPath = io::FileSystem::ResolveRelative(path, entry.value);
				        return;
			        }

			        if (entry.fullKey == "textures.occlusion" || entry.fullKey == "occlusion" || entry.fullKey == "textures.ao")
			        {
				        spec.occlusionPath = io::FileSystem::ResolveRelative(path, entry.value);
				        return;
			        }
			        if (entry.fullKey == "textures.emissive" || entry.fullKey == "emissive")
			        {
				        spec.emissivePath = io::FileSystem::ResolveRelative(path, entry.value);
			        }
		        });
		for (auto* reference: {&spec.albedoPath, &spec.normalPath, &spec.metallicRoughnessPath, &spec.occlusionPath, &spec.emissivePath, &spec.shaderVfsPath})
		{
			if (reference->starts_with("uid://"))
			{
				auto resolved = io::FileSystem::ResolveAssetPath(*reference);
				if (resolved.empty()) versionSupported = false;
				else *reference = std::move(resolved);
			}
		}

		if (ok != nullptr)
		{
			*ok = parsed && versionSupported;
		}
		return spec;
	}

		std::string ToToml(const MaterialPresetSpec& spec)
		{
			const MaterialAsset& m = spec.material;
			std::string out;

			// std::format's default for a float is the SHORTEST representation that reads
			// back as the same value, which is exactly what a round-trip needs - a fixed
			// precision either truncates or writes noise digits nobody wants in a diff.
			out += "# AetherCore material. Edit here or in the editor's material inspector.\n";
			out += "[material]\n";
			out += "schema_version = 1\n";
			out += std::format("basecolorfactor = [ {}, {}, {}, {} ]\n",
			        m.baseColorFactor.x, m.baseColorFactor.y, m.baseColorFactor.z, m.baseColorFactor.w);
			out += std::format("emissivefactor = [ {}, {}, {} ]\n", m.emissiveFactor.x, m.emissiveFactor.y, m.emissiveFactor.z);
			out += std::format("metallicfactor = {}\n", m.metallicFactor);
			out += std::format("roughnessfactor = {}\n", m.roughnessFactor);
			out += std::format("occlusionstrength = {}\n", m.occlusionStrength);
			out += std::format("alphacutoff = {}\n", m.alphaCutoff);
			// Written only when it scrolls, so the common static material's file is unchanged.
			if (m.uvScroll != glm::vec2(0.0f))
			{
				out += std::format("uvscroll = [ {}, {} ]\n", m.uvScroll.x, m.uvScroll.y);
			}
			if (m.distanceFade != glm::vec2(0.0f))
			{
				out += std::format("distancefade = [ {}, {} ]\n", m.distanceFade.x, m.distanceFade.y);
			}
			out += std::format("doublesided = {}\n", m.doubleSided ? "true" : "false");
			out += std::format("alphablend = {}\n", m.alphaBlend ? "true" : "false");
			out += std::format("alphamask = {}\n", m.alphaMask ? "true" : "false");
			out += std::format("modulatevertexcolor = {}\n", m.modulateVertexColor ? "true" : "false");
			out += std::format("receiveshadows = {}\n", m.receiveShadows ? "true" : "false");
			for (const auto& field: kAdditionalFlags)
			{
				if (m.*field.member) { out += std::format("{} = true\n", field.key); }
			}
			if (!spec.shaderVfsPath.empty())
			{
				out += std::format("shader = '{}'\n", io::FileSystem::AssetReference(spec.shaderVfsPath));
			}

			// Omitted entirely when there is no texture, rather than written empty: an empty
			// value would read back as a path and resolve to the material's own directory.
			const bool anyTexture = !spec.albedoPath.empty() || !spec.normalPath.empty()
			        || !spec.metallicRoughnessPath.empty() || !spec.occlusionPath.empty() || !spec.emissivePath.empty();
			if (anyTexture)
			{
				out += "\n[textures]\n";
				const std::pair<const char*, const std::string*> slots[] = {
				        {"albedo", &spec.albedoPath},
				        {"normal", &spec.normalPath},
				        {"metallicroughness", &spec.metallicRoughnessPath},
				        {"occlusion", &spec.occlusionPath},
				        {"emissive", &spec.emissivePath},
				};
				for (const auto& [key, value]: slots)
				{
					if (!value->empty())
					{
						out += std::format("{} = '{}'\n", key, io::FileSystem::AssetReference(*value));
					}
				}
			}

			// Last, and verbatim: the editor owns this block's contents.
			if (!spec.graphSection.empty())
			{
				out += '\n';
				out += spec.graphSection;
				out += '\n';
			}
			return out;
		}

		MaterialPresetSpec Describe(const MaterialAsset& material, const TextureRegistry& textures)
		{
			MaterialPresetSpec spec;
			spec.material = material;
			spec.shaderVfsPath = material.templateDesc.shaderVfsPath;

			const std::pair<TextureHandle, std::string*> slots[] = {
			        {material.albedoTex, &spec.albedoPath},
			        {material.normalTex, &spec.normalPath},
			        {material.metallicRoughnessTex, &spec.metallicRoughnessPath},
			        {material.occlusionTex, &spec.occlusionPath},
			        {material.emissiveTex, &spec.emissivePath},
			};
			for (const auto& [handle, out]: slots)
			{
				if (handle.IsValid())
				{
					std::string path;
					if (textures.TryGetPath(handle, path))
					{
						*out = std::move(path);
					}
				}
			}
			return spec;
		}
	} // namespace MaterialSerializer
} // namespace aether
