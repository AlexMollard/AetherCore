#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include "utils/Hash.hpp"
#include "utils/Utf8.hpp"

namespace aether::gpu
{
	// AEWG ABI 1: little-endian lengths/checksum, WGSL/source SHA-256 provenance,
	// then bounded UTF-8 WGSL. The returned view borrows the caller's package bytes.
	[[nodiscard]] inline std::expected<std::string_view, std::string> DecodeWebMaterialShader(std::span<const std::byte> bytes)
	{
		constexpr std::size_t kHeader = 88, kMaxCode = 1024 * 1024;
		if (bytes.size() < kHeader || bytes[0] != std::byte{'A'} || bytes[1] != std::byte{'E'} || bytes[2] != std::byte{'W'} || bytes[3] != std::byte{'G'})
		{
			return std::unexpected("WEB_SHADER_CONTAINER: expected a cooked AEWG material shader");
		}
		const auto integer = [&](std::size_t at, unsigned count)
		{
			std::uint64_t value = 0;
			for (unsigned index = 0; index < count; ++index)
			{
				value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8);
			}
			return value;
		};
		const auto length = integer(8, 4);
		if (integer(4, 4) != 1 || integer(12, 4) != 0 || !length || length > kMaxCode || bytes.size() - kHeader != length)
		{
			return std::unexpected("WEB_SHADER_ABI: incompatible ABI, flags or shader source length");
		}
		const std::string_view source(reinterpret_cast<const char*>(bytes.data() + kHeader), length);
		if (!IsValidUtf8(source.data(), source.size()) || source.find('\0') != std::string_view::npos || utils::Fnv1a(source) != integer(16, 8))
		{
			return std::unexpected("WEB_SHADER_INTEGRITY: malformed UTF-8 or shader checksum mismatch");
		}
		return source;
	}
} // namespace aether::gpu
