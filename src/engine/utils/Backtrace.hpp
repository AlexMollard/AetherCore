#pragma once

#include <string>
#include <string_view>

namespace aether
{

	std::string_view ShortenPath(const char* path) noexcept;

	int CaptureBacktrace(void** buffer, int maxDepth, int skipFrames = 0) noexcept;

	std::string ResolveAddress(void* addr) noexcept;

} // namespace aether
