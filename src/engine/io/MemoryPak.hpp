#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "PakFormat.hpp"
#include "utils/Expected.hpp"

namespace aether::io
{
	// An immutable, validated PAK snapshot for asynchronous browser downloads.
	class MemoryPak
	{
	public:
		static constexpr size_t kMaxPackageBytes = 256u * 1024u * 1024u;
		static constexpr size_t kMaxAssetBytes = 64u * 1024u * 1024u + 65536u;
		[[nodiscard]] static Expected<MemoryPak> Open(std::span<const std::byte> bytes, std::string_view expectedTarget);
		[[nodiscard]] Expected<std::vector<std::byte>> Read(std::string_view path) const;
		[[nodiscard]] bool Exists(std::string_view path) const;

	private:
		std::vector<std::byte> m_bytes;
		std::unordered_map<std::string, PakEntry> m_index;
		size_t m_asset_offset = 0;
	};
} // namespace aether::io
