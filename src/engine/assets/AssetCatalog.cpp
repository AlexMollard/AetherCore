#include "assets/AssetCatalog.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <unordered_set>
#include <sstream>

#include <toml++/toml.hpp>

#include "utils/TomlParse.hpp"

namespace aether
{
	namespace fs = std::filesystem;

	namespace
	{
		bool ValidUid(std::string_view s)
		{
			return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}

		bool SafeRelative(const fs::path& p)
		{
			if (p.empty() || p.is_absolute() || p.has_root_name())
			{
				return false;
			}
			for (const auto& part: p)
			{
				if (part == ".." || part == ".")
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	Expected<AssetCatalog> AssetCatalog::Parse(std::string_view text)
	{
		auto parsed = ParseTomlDocument(text);
		if (!parsed)
		{
			return std::unexpected(parsed.error());
		}
		if (!(*parsed)["version"].is_integer() || (*parsed)["version"].value_or(0) != 1)
		{
			return std::unexpected(AetherError::Asset("Unsupported asset catalog version"));
		}
		AssetCatalog catalog;
		if (auto* assets = (*parsed)["assets"].as_array())
		{
			for (auto&& node: *assets)
			{
				auto* table = node.as_table();
				if (!table)
				{
					return std::unexpected(AetherError::Asset("Invalid catalog asset"));
				}
				auto uid = (*table)["uid"].value_or(std::string{}), path = (*table)["path"].value_or(std::string{});
				if (!ValidUid(uid) || !SafeRelative(fs::path(path)) || !catalog.m_uidToPath.emplace(uid, path).second || !catalog.m_pathToUid.emplace(path, uid).second)
				{
					return std::unexpected(AetherError::Asset("Invalid or duplicate catalog identity: " + path));
				}
			}
		}
		if (auto* refs = (*parsed)["references"].as_array())
		{
			for (auto&& node: *refs)
			{
				auto* t = node.as_table();
				if (!t)
				{
					return std::unexpected(AetherError::Asset("Invalid catalog reference"));
				}
				AssetReferenceLocation r{(*t)["document"].value_or(std::string{}), (*t)["field"].value_or(std::string{}), (*t)["uid"].value_or(std::string{})};
				if (!SafeRelative(r.document) || r.field.empty() || !catalog.m_uidToPath.contains(r.uid))
				{
					return std::unexpected(AetherError::Asset("Invalid catalog dependency"));
				}
				catalog.m_references.push_back(std::move(r));
			}
		}
		return catalog;
	}

	std::string AssetCatalog::Serialize() const
	{
		toml::table root;
		root.insert("version", 1);
		toml::array assets, refs;
		std::vector<std::pair<std::string, std::string>> sorted(m_uidToPath.begin(), m_uidToPath.end());
		std::sort(sorted.begin(), sorted.end());
		for (const auto& [uid, path]: sorted)
		{
			toml::table t;
			t.insert("uid", uid);
			t.insert("path", path);
			assets.push_back(std::move(t));
		}
		for (const auto& r: m_references)
		{
			toml::table t;
			t.insert("document", r.document);
			t.insert("field", r.field);
			t.insert("uid", r.uid);
			refs.push_back(std::move(t));
		}
		root.insert("assets", std::move(assets));
		root.insert("references", std::move(refs));
		std::ostringstream out;
		out << root << '\n';
		const auto text = out.str();
		std::string crlf;
		crlf.reserve(text.size());
		for (char c: text)
		{
			if (c == '\n' && (crlf.empty() || crlf.back() != '\r'))
			{
				crlf.push_back('\r');
			}
			crlf.push_back(c);
		}
		return crlf;
	}

	Expected<void> AssetCatalog::ReadProjectDirectories()
	{
		const auto path = m_root / "ProjectSettings.toml";
		std::error_code ec;
		if (!fs::exists(path, ec))
		{
			if (ec)
			{
				return std::unexpected(AetherError::Asset("Cannot inspect project settings: " + ec.message()));
			}
			return {};
		}
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			return std::unexpected(AetherError::Asset("Cannot read project settings"));
		}
		std::ostringstream text;
		text << in.rdbuf();
		if (in.bad())
		{
			return std::unexpected(AetherError::Asset("Cannot read project settings"));
		}
		auto doc = ParseTomlDocument(text.str());
		if (!doc)
		{
			return std::unexpected(doc.error());
		}
		auto scenes = (*doc)["paths"]["scenes"].value_or(std::string{});
		auto prefabs = (*doc)["paths"]["prefabs"].value_or(std::string{});
		auto assets = (*doc)["paths"]["assets"].value_or(std::string{});
		if (!scenes.empty())
		{
			m_scenesDirectory = fs::path(scenes).lexically_normal();
		}
		if (!prefabs.empty())
		{
			m_prefabsDirectory = fs::path(prefabs).lexically_normal();
		}
		else if (!assets.empty())
		{
			m_prefabsDirectory = (fs::path(assets) / "prefabs").lexically_normal();
		}
		if (!SafeRelative(m_scenesDirectory) || !SafeRelative(m_prefabsDirectory))
		{
			return std::unexpected(AetherError::Asset("Project scene or prefab directory escapes project"));
		}
		return {};
	}

	Expected<AssetCatalog> AssetCatalog::Load(const fs::path& root)
	{
		std::ifstream input(root / "asset-catalog.toml", std::ios::binary);
		if (!input)
		{
			return std::unexpected(AetherError::Asset("Cannot read asset catalog"));
		}
		std::ostringstream text;
		text << input.rdbuf();
		if (input.bad())
		{
			return std::unexpected(AetherError::Asset("Cannot read asset catalog"));
		}
		auto catalog = Parse(text.str());
		if (!catalog)
		{
			return catalog;
		}
		std::error_code ec;
		catalog->m_root = fs::weakly_canonical(root, ec);
		if (ec)
		{
			return std::unexpected(AetherError::Asset("Cannot canonicalize project root"));
		}
		auto directories = catalog->ReadProjectDirectories();
		if (!directories)
		{
			return std::unexpected(directories.error());
		}
		std::unordered_set<std::string> seenSidecars;
		for (fs::recursive_directory_iterator it(catalog->m_root, fs::directory_options::none, ec), end; it != end && !ec; it.increment(ec))
		{
			const auto p = it->path();
			if (it->is_symlink(ec))
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_directory(ec))
			{
				const auto n = p.filename().string();
				if (n == ".git" || n == ".aether" || n == "Builds" || n == "build" || n == "cache" || n == ".cache" || n == ".asset-migration-backups" || n == "src" || n == "scripts" || n == "obj" || n == "bin" || n == "node_modules")
				{
					it.disable_recursion_pending();
				}
				continue;
			}
			if (!it->is_regular_file(ec) || p.extension() != ".uid")
			{
				continue;
			}
			auto asset = p;
			asset.replace_extension();
			if (!fs::exists(asset, ec))
			{
				if (ec)
				{
					return std::unexpected(AetherError::Asset("Cannot inspect sidecar target: " + p.string()));
				}
				continue;
			}
			if (!fs::is_regular_file(asset, ec) || ec)
			{
				return std::unexpected(AetherError::Asset("Invalid sidecar target: " + p.string()));
			}
			std::ifstream side(p, std::ios::binary);
			std::string uid;
			std::getline(side, uid);
			if (!uid.empty() && uid.back() == '\r')
			{
				uid.pop_back();
			}
			const auto path = asset.lexically_relative(catalog->m_root).generic_string();
			if (!side || !ValidUid(uid) || !seenSidecars.emplace(uid).second)
			{
				return std::unexpected(AetherError::Asset("Invalid or duplicate asset sidecar: " + p.string()));
			}
			if (auto old = catalog->m_uidToPath.find(uid); old != catalog->m_uidToPath.end())
			{
				catalog->m_pathToUid.erase(old->second);
			}
			if (auto displaced = catalog->m_pathToUid.find(path); displaced != catalog->m_pathToUid.end() && displaced->second != uid)
			{
				catalog->m_uidToPath.erase(displaced->second);
			}
			catalog->m_uidToPath.insert_or_assign(uid, path);
			catalog->m_pathToUid.insert_or_assign(path, uid);
		}
		if (ec)
		{
			return std::unexpected(AetherError::Asset("Cannot scan asset sidecars: " + ec.message()));
		}
		return catalog;
	}

	std::string AssetCatalog::Reference(std::string_view path) const
	{
		if (!path.starts_with("project://"))
		{
			return std::string(path);
		}
		auto it = m_pathToUid.find(std::string(path.substr(10)));
		return it == m_pathToUid.end() ? std::string(path) : "uid://" + it->second;
	}

	std::string AssetCatalog::Resolve(std::string_view ref) const
	{
		if (!ref.starts_with("uid://"))
		{
			return std::string(ref);
		}
		auto it = m_uidToPath.find(std::string(ref.substr(6)));
		return it == m_uidToPath.end() ? std::string{} : "project://" + it->second;
	}

	Expected<std::vector<AssetReferenceLocation>> AssetCatalog::References(std::string_view uid) const
	{
		std::string normalized;
		if (uid.starts_with("project://"))
		{
			normalized = Reference(uid);
			uid = normalized;
		}
		if (uid.starts_with("uid://"))
		{
			uid.remove_prefix(6);
		}
		std::vector<AssetReferenceLocation> result;
		if (m_root.empty())
		{
			for (const auto& r: m_references)
			{
				if (uid.empty() || r.uid == uid)
				{
					result.push_back(r);
				}
			}
			return result;
		}
		std::error_code ec;
		for (fs::recursive_directory_iterator it(m_root, fs::directory_options::none, ec), end; it != end && !ec; it.increment(ec))
		{
			const auto p = it->path();
			if (it->is_symlink(ec))
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_directory(ec))
			{
				const auto n = p.filename().string();
				if (n == ".git" || n == ".aether" || n == "Builds" || n == "build" || n == "cache" || n == ".cache" || n == ".asset-migration-backups")
				{
					it.disable_recursion_pending();
				}
				continue;
			}
			if (!it->is_regular_file(ec) || (p.extension() != ".toml" && p.extension() != ".tiles") || p.filename() == "asset-catalog.toml")
			{
				continue;
			}
			std::ifstream in(p, std::ios::binary);
			if (!in)
			{
				return std::unexpected(AetherError::Asset("Cannot read " + p.string()));
			}
			std::ostringstream buffer;
			buffer << in.rdbuf();
			const auto bytes = buffer.str();
			const auto document = p.lexically_relative(m_root).generic_string();
			auto record = [&](std::string_view text, const std::string& field)
			{
				std::string id;
				if (text.starts_with("uid://"))
				{
					id = std::string(text.substr(6));
				}
				else if (text.starts_with("project://"))
				{
					auto ref = Reference(text);
					if (ref.starts_with("uid://"))
					{
						id = ref.substr(6);
					}
				}
				else if (!text.empty() && text.find("://") == std::string_view::npos)
				{
					const bool font = field.ends_with(".font") && (field.find(".ui_text.") != std::string::npos || field.find(".ui_button.") != std::string::npos || field.find(".ui_text_box.") != std::string::npos);
					if (font && text != "Roboto" && !fs::path(text).has_parent_path())
					{
						for (const auto* suffix: {".ttf", "-Regular.ttf", ".otf", "-Regular.otf", ".fontcurves", "-Regular.fontcurves"})
						{
							auto found = m_pathToUid.find((fs::path("assets/fonts") / (std::string(text) + suffix)).generic_string());
							if (found != m_pathToUid.end())
							{
								id = found->second;
								break;
							}
						}
					}
					fs::path candidate;
					if (field.starts_with("prefab_instances[") && field.ends_with(".prefab") && !fs::path(text).has_extension())
					{
						candidate = m_prefabsDirectory / (std::string(text) + ".prefab.toml");
					}
					else if (((field.starts_with("includes[") && field.ends_with(".scene")) || (document == "ProjectSettings.toml" && (field == "app.startupScene" || field == "app.startupscene" || field == "app.startup_scene")))
					         && !fs::path(text).has_extension())
					{
						candidate = m_scenesDirectory / (std::string(text) + ".scene.toml");
					}
					else
					{
						candidate = (fs::path(document).parent_path() / fs::path(text)).lexically_normal();
					}
					if (!font)
					{
						auto found = m_pathToUid.find(candidate.generic_string());
						if (found != m_pathToUid.end())
						{
							id = found->second;
						}
					}
				}
				if (!id.empty() && (uid.empty() || id == uid))
				{
					result.push_back({document, field, std::move(id)});
				}
			};
			if (p.extension() == ".tiles")
			{
				if (bytes.size() >= 12 && bytes.substr(0, 4) == "ATLM")
				{
					const auto n = static_cast<unsigned char>(bytes[8]) | (static_cast<unsigned>(static_cast<unsigned char>(bytes[9])) << 8) | (static_cast<unsigned>(static_cast<unsigned char>(bytes[10])) << 16)
					               | (static_cast<unsigned>(static_cast<unsigned char>(bytes[11])) << 24);
					if (n <= bytes.size() - 12)
					{
						record(std::string_view(bytes).substr(12, n), "tileSetPath");
					}
				}
				continue;
			}
			auto doc = ParseTomlDocument(bytes);
			if (!doc)
			{
				return std::unexpected(doc.error());
			}
			std::function<void(const toml::node&, const std::string&)> visit;
			visit = [&](const toml::node& node, const std::string& field)
			{
				if (const auto* table = node.as_table())
				{
					for (auto&& [k, v]: *table)
					{
						const std::string name(k.str());
						visit(v, field.empty() ? name : field + "." + name);
					}
				}
				else if (const auto* array = node.as_array())
				{
					for (size_t i = 0; i < array->size(); ++i)
					{
						visit(*array->get(i), field + "[" + std::to_string(i) + "]");
					}
				}
				else if (const auto* value = node.as_string())
				{
					record(value->get(), field);
				}
			};
			visit(*doc, "");
		}
		if (ec)
		{
			return std::unexpected(AetherError::Asset("Cannot scan references: " + ec.message()));
		}
		return result;
	}
} // namespace aether
