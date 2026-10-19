#pragma once

namespace aether::io
{
	class MemoryPak;
}

namespace aether::web
{
	// The loader retains ownership until this mount has been cleared.
	void SetWebPackage(const io::MemoryPak* package, const io::MemoryPak* engine = nullptr);
} // namespace aether::web
