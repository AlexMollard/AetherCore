#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include "utils/Expected.hpp"

namespace aether::io
{
	class MemoryPak;
}

namespace aether::web
{
	// The loader retains ownership until this mount has been cleared.
	void SetWebPackage(const io::MemoryPak* package, const io::MemoryPak* engine = nullptr);
	// Individual streamed assets are owned by the mount until explicitly removed.
	Expected<void> MountWebAsset(std::string path, std::vector<std::byte> bytes);
	void UnmountWebAsset(std::string_view path);
	// Borrowed until the transport's last claim is released; empty if not mounted.
	std::span<const std::byte> FindWebAsset(std::string_view path);
} // namespace aether::web
