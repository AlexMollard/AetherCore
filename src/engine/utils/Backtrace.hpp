#pragma once

#include <string>
#include <string_view>

namespace aether
{

	// Strip the absolute path prefix so logs show a project-relative path.
	std::string_view ShortenPath(const char* path) noexcept;

	// Capture a stack backtrace into a pre-allocated buffer. Always skips this
	// function's own frame. `skipFrames` additional top frames are skipped
	// (e.g. pass 1 to also skip the direct caller). Returns the number of frames
	// written (up to `maxDepth`).
	int CaptureBacktrace(void** buffer, int maxDepth, int skipFrames = 0) noexcept;

	// Resolve a backtrace address to "symbol+offset (file:line)".
	// Falls back to "0x<hex>" if symbols are unavailable.
	std::string ResolveAddress(void* addr) noexcept;

} // namespace aether
