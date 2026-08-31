#include "material/MaterialSerializer.hpp"

#include <format>

#include "io/FileSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	namespace MaterialSerializer
	{
	namespace
	{
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

	MaterialPresetSpec Parse(std::string_view path, const std::string& text, bool* ok)
	{
		MaterialPresetSpec spec;
		spec.graphSection = ExtractGraphSection(text);

		const bool parsed = text::ParseToml(text,
		        [&spec, path](const text::IniEntry& entry)
		        {
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

		if (ok != nullptr)
		{
			*ok = parsed;
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
			out += std::format("basecolorfactor = [ {}, {}, {}, {} ]\n",
			        m.baseColorFactor.x, m.baseColorFactor.y, m.baseColorFactor.z, m.baseColorFactor.w);
			out += std::format("emissivefactor = [ {}, {}, {} ]\n", m.emissiveFactor.x, m.emissiveFactor.y, m.emissiveFactor.z);
			out += std::format("metallicfactor = {}\n", m.metallicFactor);
			out += std::format("roughnessfactor = {}\n", m.roughnessFactor);
			out += std::format("occlusionstrength = {}\n", m.occlusionStrength);
			out += std::format("alphacutoff = {}\n", m.alphaCutoff);
			out += std::format("doublesided = {}\n", m.doubleSided ? "true" : "false");
			out += std::format("alphablend = {}\n", m.alphaBlend ? "true" : "false");
			out += std::format("alphamask = {}\n", m.alphaMask ? "true" : "false");
			out += std::format("modulatevertexcolor = {}\n", m.modulateVertexColor ? "true" : "false");
			out += std::format("receiveshadows = {}\n", m.receiveShadows ? "true" : "false");
			if (!spec.shaderVfsPath.empty())
			{
				out += std::format("shader = '{}'\n", spec.shaderVfsPath);
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
						out += std::format("{} = '{}'\n", key, *value);
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
