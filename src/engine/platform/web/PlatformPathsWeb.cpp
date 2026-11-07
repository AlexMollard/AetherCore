#include "io/PlatformPaths.hpp"

namespace aether::io
{
	std::string PlatformPaths::PathToUtf8(const std::filesystem::path& path)
	{
		const auto bytes = path.u8string();
		return std::string(bytes.begin(), bytes.end());
	}
}
