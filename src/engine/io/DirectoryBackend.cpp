#include "DirectoryBackend.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <system_error>

#include "AetherExceptions.hpp"

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
		return std::filesystem::exists(Resolve(relativePath));
	}

	std::vector<std::byte> DirectoryBackend::Read(std::string_view relativePath) const
	{
		const auto fullPath = Resolve(relativePath);

		std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
		if (!file)
		{
			throw FileSystemError("Failed to open file: " + fullPath.string());
		}

		const auto size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<std::byte> buffer(static_cast<std::size_t>(size));
		if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
		{
			throw FileSystemError("Failed to read file: " + fullPath.string());
		}

		return buffer;
	}

	std::unique_ptr<std::istream> DirectoryBackend::OpenStream(std::string_view relativePath) const
	{
		const auto fullPath = Resolve(relativePath);

		auto stream = std::make_unique<std::ifstream>(fullPath, std::ios::binary);
		if (!*stream)
		{
			throw FileSystemError("Failed to open stream for file: " + fullPath.string());
		}

		return stream;
	}

	std::vector<std::string> DirectoryBackend::Glob(std::string_view pattern, const FileGlobOptions& options) const
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
} // namespace aether::io
