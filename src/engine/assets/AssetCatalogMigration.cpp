#include "assets/AssetCatalog.hpp"
#include "utils/TomlParse.hpp"
#include <algorithm>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <random>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <toml++/toml.hpp>
#include <BinaryFormats.hpp>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

namespace aether
{
	namespace fs = std::filesystem;

	namespace
	{
		namespace FileUtil
		{
			Expected<std::string> ReadText(const fs::path& p)
			{
				std::ifstream in(p, std::ios::binary);
				if (!in)
				{
					return std::unexpected(AetherError::Asset("Cannot read " + p.string()));
				}
				std::ostringstream out;
				out << in.rdbuf();
				if (in.bad())
				{
					return std::unexpected(AetherError::Asset("Cannot read " + p.string()));
				}
				return out.str();
			}

			Expected<void> WriteText(const fs::path& p, std::string_view data)
			{
				std::error_code ec;
				fs::create_directories(p.parent_path(), ec);
				if (ec)
				{
					return std::unexpected(AetherError::Asset("Cannot create directory: " + ec.message()));
				}
				std::ofstream out(p, std::ios::binary | std::ios::trunc);
				out.write(data.data(), static_cast<std::streamsize>(data.size()));
				if (!out)
				{
					return std::unexpected(AetherError::Asset("Cannot write " + p.string()));
				}
				return {};
			}
		} // namespace FileUtil

		bool replaceFile(const fs::path& from, const fs::path& to, std::error_code& ec)
		{
#ifdef _WIN32
			if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				ec.clear();
				return true;
			}
			ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			return false;
#else
			fs::rename(from, to, ec);
			return !ec;
#endif
		}

		bool safe(const fs::path& p)
		{
			if (p.empty() || p.is_absolute() || p.has_root_name())
			{
				return false;
			}
			for (const auto& x: p)
			{
				if (x == "." || x == "..")
				{
					return false;
				}
			}
			return true;
		}

		bool inside(const fs::path& root, const fs::path& p)
		{
			auto a = root.begin(), b = p.begin();
			for (; a != root.end(); ++a, ++b)
			{
				if (b == p.end() || *a != *b)
				{
					return false;
				}
			}
			return true;
		}

		std::string randomId()
		{
			std::random_device rd;
			std::ostringstream s;
			s << std::hex << std::setfill('0');
			for (int i = 0; i < 16; ++i)
			{
				s << std::setw(2) << (rd() & 255u);
			}
			return s.str();
		}

		bool valid(std::string_view s)
		{
			return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}

		bool excluded(std::string_view s)
		{
			return s == ".git" || s == ".aether" || s == "Builds" || s == "build" || s == "cache" || s == ".cache" || s == ".asset-migration-backups" || s == "src" || s == "scripts" || s == "obj" || s == "bin" || s == "node_modules";
		}

		bool authored(const fs::path& p)
		{
			auto e = p.extension().string();
			std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return e == ".toml" || e == ".tiles" || e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".webp" || e == ".dds" || e == ".texture" || e == ".ktx" || e == ".ktx2" || e == ".hdr" || e == ".exr"
			       || e == ".gltf" || e == ".glb" || e == ".fbx" || e == ".obj" || e == ".mesh" || e == ".skel" || e == ".anim" || e == ".animset" || e == ".material" || e == ".bin" || e == ".wav" || e == ".ogg" || e == ".mp3"
			       || e == ".flac" || e == ".ttf" || e == ".otf" || e == ".woff" || e == ".woff2" || e == ".fontcurves" || e == ".slang" || e == ".spv" || e == ".wshader";
		}

		bool supported(const toml::table& table, std::string_view key, int current, bool integralFloat = false)
		{
			auto node = table[key];
			if (!node)
			{
				return true;
			}
			if (node.is_integer())
			{
				const auto version = node.value<std::int64_t>();
				return version && *version >= 1 && *version <= current;
			}
			if (integralFloat && node.is_floating_point())
			{
				const auto version = node.value<double>();
				return version && std::isfinite(*version) && std::floor(*version) == *version && *version >= 1 && *version <= current;
			}
			return false;
		}

		bool validateSchema(const toml::table& root, const fs::path& rel)
		{
			const auto path = rel.generic_string();
			for (const auto& [section, key, version]: {std::tuple{"scene", "version", 18},
			             std::tuple{"prefab", "version", 1},
			             std::tuple{"atlas", "schema_version", 3},
			             std::tuple{"animation", "schema_version", 2},
			             std::tuple{"tileset", "schema_version", 2},
			             std::tuple{"material", "schema_version", 1}})
			{
				auto node = root[section];
				const bool integralFloat = std::string_view(section) == "atlas" || std::string_view(section) == "animation" || std::string_view(section) == "tileset";
				if (node && (!node.is_table() || !supported(*node.as_table(), key, version, integralFloat)))
				{
					return false;
				}
			}
			if (rel.filename() == "properties.toml" && !supported(root, "schema_version", 1))
			{
				return false;
			}
			if (!(path.ends_with(".scene.toml") || path.ends_with(".prefab.toml")))
			{
				return true;
			}
			std::function<bool(const toml::node&)> components;
			components = [&](const toml::node& node)
			{
				if (auto* table = node.as_table())
				{
					if (!supported(*table, "schema_version", 1))
					{
						return false;
					}
					for (auto&& [key, value]: *table)
					{
						if (!components(value))
						{
							return false;
						}
					}
				}
				else if (auto* array = node.as_array())
				{
					for (auto&& value: *array)
					{
						if (!components(value))
						{
							return false;
						}
					}
				}
				return true;
			};
			if (auto* entities = root["entities"].as_array(); entities && !components(*entities))
			{
				return false;
			}
			if (auto* instances = root["prefab_instances"].as_array())
			{
				for (auto&& node: *instances)
				{
					if (auto* instance = node.as_table())
					{
						if (auto* added = (*instance)["added"].as_array(); added && !components(*added))
						{
							return false;
						}
						if (auto* overrides = (*instance)["overrides"].as_array(); overrides && !components(*overrides))
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		std::string crlf(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (char c: text)
			{
				if (c == '\n' && (out.empty() || out.back() != '\r'))
				{
					out.push_back('\r');
				}
				out.push_back(c);
			}
			return out;
		}

		using Rewrite = std::function<Expected<std::string>(std::string_view, std::string_view)>;

		Expected<void> walk(toml::node& node, const std::string& field, std::string key, const Rewrite& rewrite, bool& changed)
		{
			if (auto* t = node.as_table())
			{
				for (auto&& [k, v]: *t)
				{
					std::string name(k.str());
					auto r = walk(v, field.empty() ? name : field + "." + name, name, rewrite, changed);
					if (!r)
					{
						return r;
					}
				}
			}
			else if (auto* a = node.as_array())
			{
				for (size_t i = 0; i < a->size(); ++i)
				{
					auto r = walk(*a->get(i), field + "[" + std::to_string(i) + "]", key, rewrite, changed);
					if (!r)
					{
						return r;
					}
				}
			}
			else if (auto* v = node.as_string())
			{
				auto old = v->get();
				std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				const bool named = (key == "prefab" && field.starts_with("prefab_instances[")) || (key == "scene" && (field.starts_with("includes[") || field.starts_with("stream_regions["))) || ((key == "startupscene" || key == "startup_scene") && field.starts_with("app."));
				const bool asset = key.find("texture") != std::string::npos || key.ends_with("_tex") || key == "atlas" || key == "tileset" || key == "tilemap" || key == "animation" || key == "mesh_source" || key == "surface_mesh"
				                   || key == "exclusion_mesh" || key == "prefab" || key == "include" || key == "includes" || named || (key == "path" && (field.find(".mesh.") != std::string::npos || field.starts_with("assets[")))
				                   || ((field.find("material") != std::string::npos || field.starts_with("textures."))
				                           && (key == "asset" || key == "albedo" || key == "basecolor" || key == "normal" || key == "roughness" || key == "metallic" || key == "metallicroughness" || key == "metallic_roughness" || key == "occlusion"
				                                   || key == "ao" || key == "emissive" || key == "shader"));
				const bool font = (key == "font" && (field.find(".ui_text.") != std::string::npos || field.find(".ui_button.") != std::string::npos || field.find(".ui_text_box.") != std::string::npos));
				const bool audio = (key == "clippath" || key == "clip_path") && field.find(".audio_source.") != std::string::npos;
				if ((asset || audio || font) && !old.empty() && (old.starts_with("project://") || old.starts_with("uid://") || (old.find("://") == std::string::npos && (font || named || fs::path(old).has_extension()))))
				{
					auto r = rewrite(old, field);
					if (!r)
					{
						return std::unexpected(r.error());
					}
					if (*r != old)
					{
						*v = *r;
						changed = true;
					}
				}
			}
			return {};
		}
	} // namespace

	Expected<AssetMigrationReport> AssetCatalog::Migrate(const fs::path& projectRoot, bool apply)
	{
		std::error_code ec;
		auto root = fs::weakly_canonical(projectRoot, ec);
		if (ec || !fs::is_directory(root))
		{
			return std::unexpected(AetherError::Asset("Invalid project root"));
		}
		AssetCatalog catalog;
		catalog.m_root = root;
		AssetMigrationReport report;
		std::vector<fs::path> files;
		auto directories = catalog.ReadProjectDirectories();
		if (!directories)
		{
			return std::unexpected(directories.error());
		}
		for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; it != end && !ec; it.increment(ec))
		{
			auto p = it->path();
			if (it->is_symlink(ec))
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_directory(ec))
			{
				if (excluded(p.filename().string()))
				{
					it.disable_recursion_pending();
				}
				continue;
			}
			if (it->is_regular_file(ec) && authored(p) && p.filename() != "asset-catalog.toml")
			{
				files.push_back(p.lexically_relative(root));
			}
		}
		if (ec)
		{
			return std::unexpected(AetherError::Asset("Project scan failed: " + ec.message()));
		}
		std::sort(files.begin(), files.end());
		std::unordered_map<std::string, std::string> writes;
		size_t preview = 0;
		for (const auto& rel: files)
		{
			const auto p = rel.generic_string();
			auto side = root / rel;
			side += ".uid";
			if (!fs::exists(side, ec))
			{
				continue;
			}
			if (fs::is_symlink(side, ec))
			{
				return std::unexpected(AetherError::Asset("Symlink sidecar: " + p));
			}
			auto text = FileUtil::ReadText(side);
			if (!text)
			{
				return std::unexpected(text.error());
			}
			auto uid = *text;
			while (!uid.empty() && std::isspace(static_cast<unsigned char>(uid.back())))
			{
				uid.pop_back();
			}
			if (!valid(uid) || !catalog.m_uidToPath.emplace(uid, p).second)
			{
				return std::unexpected(AetherError::Asset("Invalid or duplicate sidecar identity: " + p));
			}
			catalog.m_pathToUid.emplace(p, uid);
		}
		for (const auto& rel: files)
		{
			const auto p = rel.generic_string();
			if (catalog.m_pathToUid.contains(p))
			{
				continue;
			}
			std::string uid;
			do
			{
				if (apply)
				{
					uid = randomId();
				}
				else
				{
					std::ostringstream s;
					s << std::hex << std::setw(32) << std::setfill('0') << ++preview;
					uid = s.str();
				}
			}
			while (catalog.m_uidToPath.contains(uid));
			catalog.m_uidToPath.emplace(uid, p);
			catalog.m_pathToUid.emplace(p, uid);
			writes[p + ".uid"] = uid + "\r\n";
			report.changes.push_back("Create identity for " + p);
		}
		auto rewrite = [&](const fs::path& doc, std::string_view value, std::string_view field) -> Expected<std::string>
		{
			std::string uid;
			if (value.starts_with("uid://"))
			{
				uid = std::string(value.substr(6));
				if (!catalog.m_uidToPath.contains(uid))
				{
					return std::unexpected(AetherError::Asset("Missing identity: " + std::string(value)));
				}
			}
			else
			{
				const bool font = field.ends_with(".font") && (field.find(".ui_text.") != std::string_view::npos || field.find(".ui_button.") != std::string_view::npos || field.find(".ui_text_box.") != std::string_view::npos);
				if (font && value.find("://") == std::string_view::npos)
				{
					if (value == "Roboto")
					{
						return std::string(value);
					}
					if (fs::path(value).has_parent_path())
					{
						return std::string(value);
					}
					for (const auto* suffix: {".ttf", "-Regular.ttf", ".otf", "-Regular.otf", ".fontcurves", "-Regular.fontcurves"})
					{
						auto candidate = (fs::path("assets/fonts") / (std::string(value) + suffix)).generic_string();
						auto found = catalog.m_pathToUid.find(candidate);
						if (found != catalog.m_pathToUid.end())
						{
							catalog.m_references.push_back({doc.generic_string(), std::string(field), found->second});
							return "uid://" + found->second;
						}
					}
					return std::string(value);
				}
				fs::path p;
				if (value.starts_with("project://"))
				{
					p = fs::path(value.substr(10));
				}
				else if (field.starts_with("prefab_instances[") && field.ends_with(".prefab") && !fs::path(value).has_extension())
				{
					p = catalog.m_prefabsDirectory / (std::string(value) + ".prefab.toml");
				}
				else if ((((field.starts_with("includes[") || field.starts_with("stream_regions[")) && field.ends_with(".scene")) || (doc == "ProjectSettings.toml" && (field == "app.startupScene" || field == "app.startupscene" || field == "app.startup_scene")))
				         && !fs::path(value).has_extension())
				{
					p = catalog.m_scenesDirectory / (std::string(value) + ".scene.toml");
				}
				else
				{
					p = doc.parent_path() / fs::path(value);
				}
				p = p.lexically_normal();
				if (!safe(p))
				{
					return std::unexpected(AetherError::Asset("Reference escapes project: " + std::string(value)));
				}
				auto it = catalog.m_pathToUid.find(p.generic_string());
				if (it == catalog.m_pathToUid.end())
				{
					return std::unexpected(AetherError::Asset("Missing reference in " + doc.generic_string() + ": " + std::string(value)));
				}
				uid = it->second;
			}
			catalog.m_references.push_back({doc.generic_string(), std::string(field), uid});
			return "uid://" + uid;
		};
		for (const auto& rel: files)
		{
			if (rel.extension() != ".toml")
			{
				continue;
			}
			auto text = FileUtil::ReadText(root / rel);
			if (!text)
			{
				return std::unexpected(text.error());
			}
			auto doc = ParseTomlDocument(*text);
			if (!doc)
			{
				return std::unexpected(doc.error());
			}
			if (!validateSchema(*doc, rel))
			{
				return std::unexpected(AetherError::Asset("Unsupported or malformed schema version in " + rel.generic_string()));
			}
			bool changed = false;
			auto r = walk(
			        *doc,
			        "",
			        "",
			        [&](std::string_view v, std::string_view f) -> Expected<std::string>
			        {
				        if (rel != "ProjectSettings.toml" && (f == "app.startupScene" || f == "app.startupscene" || f == "app.startup_scene"))
				        {
					        return std::string(v);
				        }
				        return rewrite(rel, v, f);
			        },
			        changed);
			if (!r)
			{
				return std::unexpected(r.error());
			}
			if (rel.generic_string().ends_with(".scene.toml") || rel.generic_string().ends_with(".prefab.toml"))
			{
				std::function<Expected<void>(toml::node&)> keys;
				keys = [&](toml::node& node) -> Expected<void>
				{
					if (auto* table = node.as_table())
					{
						if (auto* mesh = (*table)["mesh"].as_table(); mesh && (*mesh)["kind"].value_or(std::string{}) == "model" && (*mesh)["primitive_key"].value_or(std::string{}).empty())
						{
							auto ref = (*mesh)["path"].value_or(std::string{});
							auto index = (*mesh)["index"].value_or(std::int64_t{0});
							if (ref.starts_with("uid://"))
							{
								auto it = catalog.m_uidToPath.find(ref.substr(6));
								if (it == catalog.m_uidToPath.end())
								{
									return std::unexpected(AetherError::Asset("Missing model identity in " + rel.generic_string()));
								}
								ref = it->second;
							}
							else if (ref.starts_with("project://"))
							{
								ref.erase(0, 10);
							}
							else if (ref.find("://") != std::string::npos)
							{
								return {};
							}
							auto model = fs::path(ref).lexically_normal();
							if (!safe(model) || index < 0)
							{
								return std::unexpected(AetherError::Asset("Invalid model primitive in " + rel.generic_string()));
							}
							auto cooked = root / model;
							if (cooked.extension() != ".mesh")
							{
								cooked.replace_extension(".mesh");
							}
							auto data = FileUtil::ReadText(cooked);
							auto unavailable = [&]() -> Expected<void>
							{
								if (!apply)
								{
									report.changes.push_back("Bake model to identify primitive: " + model.generic_string());
									return {};
								}
								return std::unexpected(AetherError::Asset("Bake model and retry migration; primitive identity unavailable: " + model.generic_string() + " in " + rel.generic_string()));
							};
							if (!data || data->size() < sizeof(MeshHeaderDisk))
							{
								return unavailable();
							}
							MeshHeaderDisk header{};
							std::memcpy(&header, data->data(), sizeof(header));
							if (!CheckMagic(header) || header.indexType > 1 || header.subMeshCount > 8192 || static_cast<std::uint64_t>(index) >= header.subMeshCount)
							{
								return unavailable();
							}
							std::uint64_t offset = sizeof(header) + static_cast<std::uint64_t>(header.vertexCount) * sizeof(DiskMeshVertex) + static_cast<std::uint64_t>(header.indexCount) * (header.indexType == 0 ? 2u : 4u)
							                       + static_cast<std::uint64_t>(header.subMeshCount) * sizeof(SubMeshHeaderDisk);
							if (offset > data->size())
							{
								return unavailable();
							}
							std::string key;
							for (std::int64_t i = 0; i <= index; ++i)
							{
								if (offset + 2 > data->size())
								{
									return unavailable();
								}
								std::uint16_t length = 0;
								std::memcpy(&length, data->data() + offset, 2);
								offset += 2;
								if (length > data->size() - offset)
								{
									return unavailable();
								}
								if (i == index)
								{
									key = data->substr(static_cast<std::size_t>(offset), length);
								}
								offset += length;
							}
							if (key.empty() || key == "unidentified")
							{
								return unavailable();
							}
							mesh->insert_or_assign("primitive_key", key);
							changed = true;
						}
						for (auto&& [key, value]: *table)
						{
							auto result = keys(value);
							if (!result)
							{
								return result;
							}
						}
					}
					else if (auto* array = node.as_array())
					{
						for (auto&& value: *array)
						{
							auto result = keys(value);
							if (!result)
							{
								return result;
							}
						}
					}
					return {};
				};
				auto keyed = keys(*doc);
				if (!keyed)
				{
					return std::unexpected(keyed.error());
				}
			}
			if (changed)
			{
				for (const auto& [section, key, version]: {std::tuple{"scene", "version", 18}, std::tuple{"atlas", "schema_version", 3}, std::tuple{"animation", "schema_version", 2}, std::tuple{"tileset", "schema_version", 2}})
				{
					if (auto* table = (*doc)[section].as_table())
					{
						if ((*table)[key].value_or(0) > version)
						{
							return std::unexpected(AetherError::Asset("Unsupported future asset version in " + rel.generic_string()));
						}
						table->insert_or_assign(key, version);
					}
				}
				std::ostringstream s;
				s << *doc << '\n';
				writes[rel.generic_string()] = crlf(s.str());
				report.changes.push_back("Rewrite references in " + rel.generic_string());
			}
		}
		for (const auto& rel: files)
		{
			if (rel.extension() != ".tiles")
			{
				continue;
			}
			auto bytes = FileUtil::ReadText(root / rel);
			if (!bytes)
			{
				return std::unexpected(bytes.error());
			}
			if (bytes->size() < 12 || bytes->substr(0, 4) != "ATLM")
			{
				return std::unexpected(AetherError::Asset("Invalid tile map: " + rel.generic_string()));
			}
			auto u32 = [&](size_t pos)
			{
				return static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[pos])) | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[pos + 1])) << 8)
				       | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[pos + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[pos + 3])) << 24);
			};
			const auto version = u32(4), length = u32(8);
			if ((version != 1 && version != 2) || length > 4096 || length > bytes->size() - 12)
			{
				return std::unexpected(AetherError::Asset("Invalid tile map header: " + rel.generic_string()));
			}
			auto old = bytes->substr(12, length);
			if (old.empty() || old.starts_with("engine://"))
			{
				continue;
			}
			auto ref = rewrite(rel, old, "tileSetPath");
			if (!ref)
			{
				return std::unexpected(ref.error());
			}
			if (*ref != old)
			{
				(*bytes)[4] = 2;
				(*bytes)[5] = 0;
				(*bytes)[6] = 0;
				(*bytes)[7] = 0;
				auto n = static_cast<std::uint32_t>(ref->size());
				for (size_t i = 0; i < 4; ++i)
				{
					(*bytes)[8 + i] = static_cast<char>((n >> (i * 8)) & 255);
				}
				bytes->replace(12, length, *ref);
				writes[rel.generic_string()] = *bytes;
				report.changes.push_back("Rewrite tile map " + rel.generic_string());
			}
		}
		writes["asset-catalog.toml"] = catalog.Serialize();
		report.changes.push_back("Rebuild asset catalog");
		if (!apply)
		{
			return report;
		}
		auto token = randomId();
		auto backup = root / ".asset-migration-backups" / token, stage = root / ".asset-migration-backups" / (token + ".stage");
		report.backupDirectory = backup;
		for (const auto& [p, data]: writes)
		{
			auto r = FileUtil::WriteText(stage / fs::path(p), data);
			if (!r)
			{
				fs::remove_all(stage, ec);
				return std::unexpected(r.error());
			}
			if (fs::exists(root / fs::path(p), ec))
			{
				fs::create_directories((backup / fs::path(p)).parent_path(), ec);
				if (!ec)
				{
					fs::copy_file(root / fs::path(p), backup / fs::path(p), fs::copy_options::none, ec);
				}
				if (ec)
				{
					fs::remove_all(stage, ec);
					return std::unexpected(AetherError::Asset("Migration backup failed"));
				}
			}
		}
		std::vector<std::string> committed;
		for (const auto& [p, data]: writes)
		{
			if (!replaceFile(stage / fs::path(p), root / fs::path(p), ec))
			{
				std::string failures;
				for (auto it = committed.rbegin(); it != committed.rend(); ++it)
				{
					std::error_code e;
					auto original = backup / fs::path(*it), destination = root / fs::path(*it);
					if (fs::exists(original, e))
					{
						auto restore = stage / (fs::path(*it).generic_string() + ".restore");
						fs::create_directories(restore.parent_path(), e);
						if (!e)
						{
							fs::copy_file(original, restore, fs::copy_options::overwrite_existing, e);
						}
						if (!e)
						{
							replaceFile(restore, destination, e);
						}
					}
					else if (!e)
					{
						fs::remove(destination, e);
					}
					if (e)
					{
						failures += " " + *it + ": " + e.message();
					}
				}
				fs::remove_all(stage, ec);
				return std::unexpected(AetherError::Asset("Migration commit failed; backup at " + backup.string() + (failures.empty() ? "; rollback succeeded" : "; rollback failed:" + failures)));
			}
			committed.push_back(p);
		}
		fs::remove_all(stage, ec);
		return report;
	}

	Expected<void> AssetCatalog::Move(const fs::path& source, const fs::path& target)
	{
		if (m_root.empty() || !safe(source) || !safe(target))
		{
			return std::unexpected(AetherError::Asset("Move requires a loaded catalog and relative paths"));
		}
		const auto isMetadata = [](const fs::path& path)
		{
			auto name = path.generic_string();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name == "asset-catalog.toml" || fs::path(name).extension() == ".uid";
		};
		if (isMetadata(source) || isMetadata(target))
		{
			return std::unexpected(AetherError::Asset("Catalog metadata cannot be moved independently"));
		}
		std::error_code ec;
		auto src = m_root / source, dst = m_root / target;
		auto real = fs::weakly_canonical(src, ec);
		if (ec || !inside(m_root, real) || fs::is_symlink(src, ec) || !fs::exists(src, ec))
		{
			return std::unexpected(AetherError::Asset("Invalid source"));
		}
		auto parent = fs::weakly_canonical(dst.parent_path(), ec);
		if (ec || !inside(m_root, parent) || !fs::is_directory(parent, ec) || fs::exists(dst, ec) || inside(real, parent))
		{
			return std::unexpected(AetherError::Asset("Invalid target"));
		}
		if (fs::is_directory(src, ec))
		{
			for (fs::recursive_directory_iterator it(src, ec), end; it != end && !ec; it.increment(ec))
			{
				if (it->is_symlink(ec))
				{
					return std::unexpected(AetherError::Asset("Folder contains a symlink"));
				}
			}
		}
		if (ec)
		{
			return std::unexpected(AetherError::Asset("Cannot inspect move"));
		}
		fs::path curveSrc, curveDst, curveSide, curveSideDst;
		bool hasCurve = false, hasCurveSide = false;
		if (src.extension() == ".ttf" || src.extension() == ".otf")
		{
			curveSrc = src;
			curveSrc.replace_extension(".fontcurves");
			curveDst = dst;
			curveDst.replace_extension(".fontcurves");
			hasCurve = curveSrc != curveDst && fs::exists(curveSrc, ec);
			if (ec)
			{
				return std::unexpected(AetherError::Asset("Cannot inspect font companion: " + ec.message()));
			}
			if (hasCurve)
			{
				if (fs::is_symlink(curveSrc, ec) || !fs::is_regular_file(curveSrc, ec) || fs::exists(curveDst, ec))
				{
					return std::unexpected(AetherError::Asset("Invalid or colliding font companion"));
				}
				curveSide = curveSrc;
				curveSide += ".uid";
				curveSideDst = curveDst;
				curveSideDst += ".uid";
				hasCurveSide = fs::exists(curveSide, ec);
				if (ec || (hasCurveSide && fs::is_symlink(curveSide, ec)) || fs::exists(curveSideDst, ec))
				{
					return std::unexpected(AetherError::Asset("Invalid or colliding font companion sidecar"));
				}
			}
		}
		for (const auto& [uid, path]: m_uidToPath)
		{
			const fs::path gltf = m_root / fs::path(path);
			if (gltf.extension() != ".gltf")
			{
				continue;
			}
			auto text = FileUtil::ReadText(gltf);
			if (!text)
			{
				return std::unexpected(text.error());
			}
			auto json = nlohmann::json::parse(*text, nullptr, false);
			if (json.is_discarded() || !json.is_object())
			{
				return std::unexpected(AetherError::Asset("Cannot inspect glTF " + path));
			}
			for (const auto* group: {"buffers", "images"})
			{
				if (!json.contains(group))
				{
					continue;
				}
				if (!json[group].is_array())
				{
					return std::unexpected(AetherError::Asset("Invalid glTF dependencies: " + path));
				}
				for (const auto& entry: json[group])
				{
					if (!entry.is_object() || !entry.contains("uri"))
					{
						continue;
					}
					if (!entry["uri"].is_string())
					{
						return std::unexpected(AetherError::Asset("Invalid glTF URI: " + path));
					}
					const auto uri = entry["uri"].get<std::string>();
					if (uri.starts_with("data:"))
					{
						continue;
					}
					if (uri.find('%') != std::string::npos || uri.find("://") != std::string::npos || fs::path(uri).is_absolute())
					{
						return std::unexpected(AetherError::Asset("Unsupported glTF URI: " + path));
					}
					auto dependency = (gltf.parent_path() / fs::path(uri)).lexically_normal();
					if (!inside(m_root, dependency))
					{
						return std::unexpected(AetherError::Asset("glTF dependency outside project: " + path));
					}
					const bool documentMoves = inside(src, gltf), dependencyMoves = inside(src, dependency);
					if (documentMoves != dependencyMoves)
					{
						return std::unexpected(AetherError::Asset("Move would break relative glTF dependency: " + path + " -> " + uri));
					}
				}
			}
		}
		auto refs = References({});
		if (!refs)
		{
			return std::unexpected(refs.error());
		}
		std::vector<AssetReferenceLocation> refreshed = std::move(*refs);
		std::unordered_map<std::string, std::string> documents;
		for (const auto& ref: refreshed)
		{
			auto asset = m_uidToPath.find(ref.uid);
			if (asset == m_uidToPath.end())
			{
				return std::unexpected(AetherError::Asset("Missing identity in " + ref.document + ": " + ref.uid));
			}
			if (inside(src, m_root / fs::path(asset->second)) || (hasCurve && m_root / fs::path(asset->second) == curveSrc) || inside(src, m_root / fs::path(ref.document)))
			{
				auto it = documents.find(ref.document);
				if (it == documents.end())
				{
					auto text = FileUtil::ReadText(m_root / fs::path(ref.document));
					if (!text)
					{
						return std::unexpected(text.error());
					}
					it = documents.emplace(ref.document, std::move(*text)).first;
				}
				if (it->second.find("uid://" + ref.uid) == std::string::npos)
				{
					return std::unexpected(AetherError::Asset("Move would break an unmigrated reference in " + ref.document + " field " + ref.field));
				}
			}
		}
		auto from = source.generic_string(), to = target.generic_string();
		AssetCatalog updated = *this;
		updated.m_pathToUid.clear();
		bool found = false;
		for (auto& [uid, path]: updated.m_uidToPath)
		{
			if (path == from || (path.size() > from.size() && path.starts_with(from) && path[from.size()] == '/'))
			{
				path = to + path.substr(from.size());
				found = true;
			}
			if (!updated.m_pathToUid.emplace(path, uid).second)
			{
				return std::unexpected(AetherError::Asset("Catalog collision"));
			}
		}
		// Scripts, notes and empty folders have no asset identity. They still use the
		// validated move transaction, while authored assets must be migrated first.
		if (!found)
		{
			if (!fs::is_directory(src, ec) && authored(src))
			{
				return std::unexpected(AetherError::Asset("Source is not indexed"));
			}
			if (fs::is_directory(src, ec) && !excluded(src.filename().string()))
			{
				for (fs::recursive_directory_iterator it(src, ec), end; it != end && !ec; it.increment(ec))
				{
					if (it->is_directory(ec) && excluded(it->path().filename().string()))
					{
						it.disable_recursion_pending();
					}
					else if (it->is_regular_file(ec) && authored(it->path()))
					{
						return std::unexpected(AetherError::Asset("Folder contains an unindexed asset"));
					}
				}
			}
			if (ec)
			{
				return std::unexpected(AetherError::Asset("Cannot inspect move"));
			}
		}
		for (auto& ref: updated.m_references)
		{
			if (ref.document == from || (ref.document.size() > from.size() && ref.document.starts_with(from) && ref.document[from.size()] == '/'))
			{
				ref.document = to + ref.document.substr(from.size());
			}
		}
		if (hasCurve)
		{
			const auto oldPath = curveSrc.lexically_relative(m_root).generic_string(), newPath = curveDst.lexically_relative(m_root).generic_string();
			auto entry = updated.m_pathToUid.find(oldPath);
			if (entry != updated.m_pathToUid.end())
			{
				const auto uid = entry->second;
				if (updated.m_pathToUid.contains(newPath))
				{
					return std::unexpected(AetherError::Asset("Font companion catalog collision"));
				}
				updated.m_pathToUid.erase(entry);
				updated.m_pathToUid.emplace(newPath, uid);
				updated.m_uidToPath[uid] = newPath;
			}
		}
		updated.m_references = std::move(refreshed);
		for (auto& ref: updated.m_references)
		{
			if (ref.document == from || (ref.document.size() > from.size() && ref.document.starts_with(from) && ref.document[from.size()] == '/'))
			{
				ref.document = to + ref.document.substr(from.size());
			}
		}
		auto side = src;
		side += ".uid";
		auto sideDst = dst;
		sideDst += ".uid";
		if (fs::exists(sideDst, ec))
		{
			return std::unexpected(AetherError::Asset("Sidecar collision"));
		}
		auto tmp = m_root / ("asset-catalog." + randomId() + ".tmp");
		auto result = FileUtil::WriteText(tmp, updated.Serialize());
		if (!result)
		{
			return result;
		}
		std::vector<std::pair<fs::path, fs::path>> moves{{src, dst}};
		if (fs::exists(side, ec))
		{
			moves.emplace_back(side, sideDst);
		}
		if (hasCurve)
		{
			moves.emplace_back(curveSrc, curveDst);
			if (hasCurveSide)
			{
				moves.emplace_back(curveSide, curveSideDst);
			}
		}
		std::size_t moved = 0;
		auto rollback = [&]()
		{
			std::string errors;
			while (moved > 0)
			{
				--moved;
				std::error_code e;
				fs::rename(moves[moved].second, moves[moved].first, e);
				if (e)
				{
					errors += " " + moves[moved].first.string() + ": " + e.message();
				}
			}
			return errors;
		};
		for (const auto& [fromPath, toPath]: moves)
		{
			fs::rename(fromPath, toPath, ec);
			if (ec)
			{
				auto errors = rollback();
				std::error_code ignored;
				fs::remove(tmp, ignored);
				return std::unexpected(AetherError::Asset("Move failed" + (errors.empty() ? std::string("; rollback succeeded") : "; rollback failed:" + errors)));
			}
			++moved;
		}
		if (!replaceFile(tmp, m_root / "asset-catalog.toml", ec))
		{
			auto errors = rollback();
			std::error_code ignored;
			fs::remove(tmp, ignored);
			return std::unexpected(AetherError::Asset("Catalog commit failed" + (errors.empty() ? std::string("; rollback succeeded") : "; rollback failed:" + errors)));
		}
		*this = std::move(updated);
		return {};
	}
} // namespace aether
