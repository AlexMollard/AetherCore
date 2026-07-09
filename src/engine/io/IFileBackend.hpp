#pragma once

#include <cstddef>
#include <expected>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "FileGlobOptions.hpp"
#include "utils/Expected.hpp"

namespace aether::io
{
	class IFileBackend
	{
	public:
		virtual ~IFileBackend() = default;

		[[nodiscard]] virtual bool Exists(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual Expected<std::vector<std::byte>> Read(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions& options) const = 0;

		// Write data to a file at the given relative path.
		// Creates parent directories if needed.
		[[nodiscard]] virtual Expected<void> Write(std::string_view relativePath, std::span<const std::byte> data) const = 0;
	};
} // namespace aether::io
