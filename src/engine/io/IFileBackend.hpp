#pragma once

#include <cstddef>
#include <cstdint>
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

		// Opaque stamp identifying the current content of `relativePath` (mtime+size on loose
	// directories, the entry's content hash in a pak). 0 = unknown. Long-lived in-process
	// asset caches key on it so a re-extract/re-bake invalidates without rereading the file.
	[[nodiscard]] virtual std::uint64_t ContentStamp(std::string_view) const
	{
		return 0;
	}

	[[nodiscard]] virtual Expected<void> Write(std::string_view relativePath, std::span<const std::byte> data) const = 0;
	};
} // namespace aether::io
