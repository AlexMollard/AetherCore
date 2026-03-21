#pragma once

#include <cstddef>
#include <istream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "FileGlobOptions.hpp"

namespace meow::io
{
	class IFileBackend
	{
	public:
		virtual ~IFileBackend() = default;

		[[nodiscard]] virtual bool Exists(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual std::vector<std::byte> Read(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual std::unique_ptr<std::istream> OpenStream(std::string_view relativePath) const = 0;
		[[nodiscard]] virtual std::vector<std::string> Glob(std::string_view pattern, const FileGlobOptions& options) const = 0;
	};
}
