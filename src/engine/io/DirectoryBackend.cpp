#include "DirectoryBackend.hpp"

#include "GlobMatch.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <regex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "utils/AetherExceptions.hpp"
#include "utils/Expected.hpp"
#include "utils/StringUtils.hpp"

namespace aether::io
{
	namespace
	{
		std::string NormalizePath(std::string path)
		{
			std::ranges::replace(path, '\\', '/');
			return path;
		}

	} // namespace

	DirectoryBackend::DirectoryBackend(std::filesystem::path rootPath)
	      : m_rootPath(std::move(rootPath))
	{
	}

	bool DirectoryBackend::Exists(std::string_view relativePath) const
	{
		return ResolveInsensitive(relativePath).has_value();
	}

	Expected<std::vector<std::byte>> DirectoryBackend::Read(std::string_view relativePath) const
	{
		std::string bestMatch;
		auto fullPath = ResolveInsensitive(relativePath, &bestMatch);
		if (!fullPath)
		{
			std::string msg = "failed to open file: " + std::string(relativePath);
			if (!bestMatch.empty())
			{
				msg += ". Did you mean '" + bestMatch + "'?";
			}
			else
			{
				auto suggestions = CollectDidYouMean(relativePath);
				if (!suggestions.empty())
				{
					msg += ". Did you mean: ";
					for (std::size_t i = 0; i < suggestions.size(); ++i)
					{
						if (i > 0)
						{
							msg += ", ";
						}
						msg += "'" + suggestions[i] + "'";
					}
					msg += "?";
				}
			}
			AE_UNEXPECTED(AetherError::FileSystem(std::move(msg)));
		}

		std::ifstream file(*fullPath, std::ios::binary | std::ios::ate);
		if (!file)
		{
			AE_UNEXPECTED(AetherError::FileSystem("failed to open file: " + fullPath->string()));
		}

		const auto size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<std::byte> buffer(static_cast<std::size_t>(size));
		if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
		{
			AE_UNEXPECTED(AetherError::FileSystem("failed to read file: " + fullPath->string()));
		}

		return buffer;
	}

	Expected<std::unique_ptr<std::istream>> DirectoryBackend::OpenStream(std::string_view relativePath) const
	{
		std::string bestMatch;
		auto fullPath = ResolveInsensitive(relativePath, &bestMatch);
		if (!fullPath)
		{
			std::string msg = "failed to open stream for file: " + std::string(relativePath);
			if (!bestMatch.empty())
			{
				msg += ". Did you mean '" + bestMatch + "'?";
			}
			else
			{
				auto suggestions = CollectDidYouMean(relativePath);
				if (!suggestions.empty())
				{
					msg += ". Did you mean: ";
					for (std::size_t i = 0; i < suggestions.size(); ++i)
					{
						if (i > 0)
						{
							msg += ", ";
						}
						msg += "'" + suggestions[i] + "'";
					}
					msg += "?";
				}
			}
			AE_UNEXPECTED(AetherError::FileSystem(std::move(msg)));
		}

		auto stream = std::make_unique<std::ifstream>(*fullPath, std::ios::binary);
		if (!*stream)
		{
			AE_UNEXPECTED(AetherError::FileSystem("failed to open stream for file: " + fullPath->string()));
		}

		return stream;
	}

	Expected<std::vector<std::string>> DirectoryBackend::Glob(std::string_view pattern, const FileGlobOptions& options) const
	{
		std::vector<std::string> matches;

		if (!std::filesystem::exists(m_rootPath))
		{
			return matches;
		}

		const auto normalizedPattern = NormalizePath(std::string(pattern));
		const auto regexFlags = options.caseSensitive ? std::regex_constants::ECMAScript : (std::regex_constants::ECMAScript | std::regex_constants::icase);
		const std::regex matcher(GlobToRegex(normalizedPattern), regexFlags);
		const std::filesystem::path root = m_rootPath.lexically_normal();

		std::error_code errorCode;
		if (options.recursive)
		{
			// Only the directory named by the pattern's wildcard-free prefix can hold a
			// match, so walk that instead of the whole mount: a project with tens of
			// thousands of extracted files took ~2.4 s per call walking everything (and
			// asking the OS to canonicalise every entry) - Twinsanity's level build made
			// 18 such calls. Patterns with '.'/'..' segments keep the full walk, which
			// never matches them, so they behave exactly as before.
			// On a case-insensitive filesystem a missing path is missing in every casing; on a
			// case-sensitive one a case-insensitive glob may still match a differently-cased
			// directory, so that case falls back to the full walk.
#if defined(_WIN32)
			const bool missingMeansNoMatch = true;
#else
			const bool missingMeansNoMatch = options.caseSensitive;
#endif
			std::filesystem::path walkRoot = root;
			const bool dotSegment = std::ranges::any_of(std::filesystem::path(normalizedPattern), [](const std::filesystem::path& s) { return s == "." || s == ".."; });
			const std::size_t wildcard = normalizedPattern.find_first_of("*?");
			if (!dotSegment && wildcard == std::string::npos)
			{
				// A literal path matches itself or nothing.
				const auto full = options.caseSensitive ? Resolve(normalizedPattern) : ResolveInsensitive(normalizedPattern);
				if (full.has_value() && std::filesystem::exists(*full, errorCode))
				{
					const bool isDirectory = std::filesystem::is_directory(*full, errorCode);
					const auto relativeString = NormalizePath(full->lexically_normal().lexically_relative(root).generic_string());
					if ((!isDirectory || options.includeDirectories) && std::regex_match(relativeString, matcher))
					{
						matches.push_back(relativeString);
					}
					return matches;
				}
				errorCode.clear();
				if (missingMeansNoMatch)
				{
					return matches;
				}
			}
			if (!dotSegment)
			{
				const std::size_t slash = normalizedPattern.rfind('/', wildcard);
				if (slash != std::string::npos)
				{
					const auto dir = Resolve(std::string_view(normalizedPattern).substr(0, slash));
					if (!dir.has_value())
					{
						return matches; // outside the mount root: nothing there can match
					}
					if (std::filesystem::is_directory(*dir, errorCode))
					{
						walkRoot = *dir;
					}
					else if (missingMeansNoMatch)
					{
						return matches;
					}
					errorCode.clear();
				}
			}

			for (std::filesystem::recursive_directory_iterator it(walkRoot, errorCode), end; it != end; ++it)
			{
				if (errorCode)
				{
					errorCode.clear();
					continue;
				}

				if (!options.includeDirectories && it->is_directory())
				{
					continue;
				}

				// Lexical: every entry path starts with walkRoot, itself under root. The
				// filesystem::relative this replaced canonicalised each entry via the OS.
				const auto relativePath = it->path().lexically_relative(root);
				if (relativePath.empty())
				{
					continue;
				}

				const auto relativeString = NormalizePath(relativePath.generic_string());
				if (std::regex_match(relativeString, matcher))
				{
					matches.push_back(relativeString);
				}
			}
		}
		else
		{
			for (std::filesystem::directory_iterator it(m_rootPath, errorCode), end; it != end; ++it)
			{
				if (errorCode)
				{
					errorCode.clear();
					continue;
				}

				if (!options.includeDirectories && it->is_directory())
				{
					continue;
				}

				const auto relativePath = std::filesystem::relative(it->path(), m_rootPath, errorCode);
				if (errorCode)
				{
					errorCode.clear();
					continue;
				}

				const auto relativeString = NormalizePath(relativePath.generic_string());
				if (std::regex_match(relativeString, matcher))
				{
					matches.push_back(relativeString);
				}
			}
		}

		std::ranges::sort(matches);
		return matches;
	}

	std::optional<std::filesystem::path> DirectoryBackend::Resolve(std::string_view relativePath) const
	{
		// The mount root is a security boundary: relativePath arrives from scene and
		// asset data as often as from engine code, and without a containment check a
		// 'project://../secrets' style path reads and writes real files outside the
		// root (Read, OpenStream and Write all resolve through here). The check is
		// lexical - no symlink resolution - which is sound because the normalized
		// candidate returned below is exactly the path handed to the OS, so what is
		// validated is what gets opened, even on a case-insensitive filesystem.
		const std::filesystem::path rel(relativePath);
		if (rel.is_absolute() || rel.has_root_name())
		{
			return std::nullopt;
		}

		const std::filesystem::path root = m_rootPath.lexically_normal();
		const std::filesystem::path candidate = (m_rootPath / rel).lexically_normal();
		auto candidateIt = candidate.begin();
		for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++candidateIt)
		{
			if (candidateIt == candidate.end() || *candidateIt != *rootIt)
			{
				return std::nullopt;
			}
		}
		return candidate;
	}

	std::optional<std::filesystem::path> DirectoryBackend::ResolveInsensitive(std::string_view relativePath, std::string* bestMatch) const
	{
		auto fullPath = Resolve(relativePath);
		if (!fullPath)
		{
			if (bestMatch)
			{
				*bestMatch = {};
			}
			return std::nullopt;
		}

		std::error_code ec;
		if (std::filesystem::exists(*fullPath, ec))
		{
			return *fullPath;
		}

		const auto parent = fullPath->parent_path();
		const std::string targetFilename = std::filesystem::path(relativePath).filename().generic_string();

		if (!std::filesystem::exists(parent, ec))
		{
			if (bestMatch)
			{
				*bestMatch = {};
			}
			return std::nullopt;
		}

		for (std::filesystem::directory_iterator it(parent, ec), end; it != end; ++it)
		{
			const std::string candidate = it->path().filename().generic_string();
			if (utils::IEq(candidate, targetFilename))
			{
				if (bestMatch)
				{
					auto parentVfs = std::filesystem::path(relativePath).parent_path().generic_string();
					if (!parentVfs.empty())
					{
						parentVfs += '/';
						parentVfs += candidate;
					}
					*bestMatch = parentVfs.empty() ? candidate : parentVfs;
				}
				return it->path();
			}
		}

		if (bestMatch)
		{
			*bestMatch = {};
			int bestDist = std::numeric_limits<int>::max();
			for (std::filesystem::directory_iterator it(parent, ec), end; it != end; ++it)
			{
				const std::string candidate = it->path().filename().generic_string();
				const int d = utils::Levenshtein(targetFilename, candidate);
				if (d < bestDist && d <= static_cast<int>(targetFilename.size()) / 2 + 1)
				{
					bestDist = d;
					auto parentVfs = std::filesystem::path(relativePath).parent_path().generic_string();
					if (!parentVfs.empty())
					{
						parentVfs += '/';
						parentVfs += candidate;
					}
					*bestMatch = parentVfs.empty() ? candidate : parentVfs;
				}
			}
		}

		return std::nullopt;
	}

	// mtime+size - enough to notice a re-extract/re-bake without rereading the file.
	std::uint64_t DirectoryBackend::ContentStamp(std::string_view relativePath) const
	{
		const auto full = ResolveInsensitive(relativePath);
		if (!full.has_value())
		{
			return 0;
		}
		std::error_code ec;
		const auto mtime = std::filesystem::last_write_time(*full, ec);
		if (ec)
		{
			return 0;
		}
		const auto size = std::filesystem::file_size(*full, ec);
		return static_cast<std::uint64_t>(mtime.time_since_epoch().count()) * 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(ec ? 0 : size) + 0x165667B19E3779F9ull);
	}

	Expected<void> DirectoryBackend::Write(std::string_view relativePath, std::span<const std::byte> data) const
	{
		auto fullPath = Resolve(relativePath);
		if (!fullPath)
		{
			AE_UNEXPECTED(AetherError::FileSystem("path escapes the mount root: " + std::string(relativePath)));
		}

		std::error_code ec;
		auto parent = fullPath->parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				AE_UNEXPECTED(AetherError::FileSystem(std::format("failed to create directories for '{}': {}", fullPath->string(), ec.message())));
			}
		}

		std::ofstream out(*fullPath, std::ios::binary | std::ios::trunc);
		if (!out)
		{
			AE_UNEXPECTED(AetherError::FileSystem("failed to open file for writing: " + fullPath->string()));
		}

		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		out.close();

		if (out.fail())
		{
			AE_UNEXPECTED(AetherError::FileSystem("failed to write file: " + fullPath->string()));
		}

		return {};
	}

	std::vector<std::string> DirectoryBackend::CollectDidYouMean(std::string_view relativePath, int maxSuggestions) const
	{
		const auto fullPath = Resolve(relativePath);
		if (!fullPath)
		{
			return {};
		}
		const auto parent = fullPath->parent_path();
		const std::string targetFilename = std::filesystem::path(relativePath).filename().generic_string();

		std::error_code ec;
		if (!std::filesystem::exists(parent, ec))
		{
			return {};
		}

		using Pair = std::pair<int, std::string>;
		auto cmp = [](const Pair& a, const Pair& b)
		{
			return a.first > b.first;
		};
		std::priority_queue<Pair, std::vector<Pair>, decltype(cmp)> pq(cmp);

		for (std::filesystem::directory_iterator it(parent, ec), end; it != end; ++it)
		{
			const std::string candidate = it->path().filename().generic_string();
			const int d = utils::Levenshtein(targetFilename, candidate);
			pq.emplace(d, candidate);
			if (std::cmp_greater(pq.size(), maxSuggestions))
			{
				pq.pop();
			}
		}

		std::vector<std::string> results;
		while (!pq.empty())
		{
			const auto& top = pq.top();
			const auto parentVfs = std::filesystem::path(relativePath).parent_path().generic_string();
			results.push_back(parentVfs.empty() ? top.second : parentVfs + "/" + top.second);
			pq.pop();
		}
		std::ranges::reverse(results);
		return results;
	}
} // namespace aether::io
