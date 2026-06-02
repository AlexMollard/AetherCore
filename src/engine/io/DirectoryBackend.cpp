#include "DirectoryBackend.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <regex>
#include <string_view>
#include <system_error>
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
			std::replace(path.begin(), path.end(), '\\', '/');
			return path;
		}

		std::string GlobToRegex(std::string_view pattern)
		{
			std::string regex;
			regex.reserve(pattern.size() * 2 + 2);
			regex += '^';

			for (std::size_t i = 0; i < pattern.size(); ++i)
			{
				const char c = pattern[i];
				if (c == '*')
				{
					const bool isDoubleStar = (i + 1 < pattern.size() && pattern[i + 1] == '*');
					if (isDoubleStar)
					{
						const bool followedBySlash = (i + 2 < pattern.size() && pattern[i + 2] == '/');
						if (followedBySlash)
						{
							// "**/" should match zero or more directory segments.
							regex += "(?:.*/)?";
							i += 2;
						}
						else
						{
							regex += ".*";
							++i;
						}
					}
					else
					{
						regex += "[^/]*";
					}
					continue;
				}

				if (c == '?')
				{
					regex += "[^/]";
					continue;
				}

				if (c == '.' || c == '^' || c == '$' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\')
				{
					regex += '\\';
				}

				regex += c;
			}

			regex += '$';
			return regex;
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

		std::error_code errorCode;
		if (options.recursive)
		{
			for (std::filesystem::recursive_directory_iterator it(m_rootPath, errorCode), end; it != end; ++it)
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

		std::sort(matches.begin(), matches.end());
		return matches;
	}

	std::filesystem::path DirectoryBackend::Resolve(std::string_view relativePath) const
	{
		return m_rootPath / relativePath;
	}

	std::optional<std::filesystem::path> DirectoryBackend::ResolveInsensitive(std::string_view relativePath, std::string* bestMatch) const
	{
		const auto fullPath = Resolve(relativePath);

		// Exact match (fast path – works on case-insensitive filesystems).
		std::error_code ec;
		if (std::filesystem::exists(fullPath, ec))
		{
			return fullPath;
		}

		// Case-insensitive fallback: scan the parent directory.
		const auto parent = fullPath.parent_path();
		const std::string targetFilename = std::filesystem::path(relativePath).filename().generic_string();

		if (!std::filesystem::exists(parent, ec))
		{
			// Can't scan – pass through to fuzzy search below via bestMatch.
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
					// Reconstruct the virtual path with the correct casing.
					const auto parentVfs = std::filesystem::path(relativePath).parent_path().generic_string();
					*bestMatch = parentVfs.empty() ? candidate : parentVfs + "/" + candidate;
				}
				return it->path();
			}
		}

		// Fuzzy (Levenshtein) – only on explicit request.
		if (bestMatch)
		{
			*bestMatch = {};
			int bestDist = (std::numeric_limits<int>::max)();
			for (std::filesystem::directory_iterator it(parent, ec), end; it != end; ++it)
			{
				const std::string candidate = it->path().filename().generic_string();
				const int d = utils::Levenshtein(targetFilename, candidate);
				if (d < bestDist && d <= static_cast<int>(targetFilename.size()) / 2 + 1)
				{
					bestDist = d;
					const auto parentVfs = std::filesystem::path(relativePath).parent_path().generic_string();
					*bestMatch = parentVfs.empty() ? candidate : parentVfs + "/" + candidate;
				}
			}
		}

		return std::nullopt;
	}

	std::vector<std::string> DirectoryBackend::CollectDidYouMean(std::string_view relativePath, int maxSuggestions) const
	{
		const auto fullPath = Resolve(relativePath);
		const auto parent = fullPath.parent_path();
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
			if (static_cast<int>(pq.size()) > maxSuggestions)
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
		std::reverse(results.begin(), results.end());
		return results;
	}
} // namespace aether::io
