#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace aether::gpu
{
	inline constexpr std::string_view kGraphPrepassEntry = "graphPrepassMain";

	// Read only the bounded instruction stream: published materials need no editor
	// graph metadata or filename convention to advertise their optional companion.
	[[nodiscard]] inline std::expected<bool, std::string> HasSpirvFragmentEntry(std::span<const std::byte> bytes, std::string_view entry)
	{
		if (bytes.size() < 20 || bytes.size() > 32 * 1024 * 1024 || bytes.size() % 4 != 0)
		{
			return std::unexpected("Invalid SPIR-V module length");
		}
		const auto word = [&](std::size_t index)
		{
			std::uint32_t value = 0;
			for (unsigned i = 0; i < 4; ++i)
			{
				value |= std::uint32_t(std::to_integer<unsigned char>(bytes[index * 4 + i])) << (8 * i);
			}
			return value;
		};
		if (word(0) != 0x07230203 || word(3) == 0 || word(4) != 0)
		{
			return std::unexpected("Invalid SPIR-V header");
		}
		bool found = false;
		for (std::size_t at = 5; at < bytes.size() / 4;)
		{
			const auto instruction = word(at);
			const auto count = instruction >> 16;
			if (!count || count > bytes.size() / 4 - at)
			{
				return std::unexpected("Truncated or zero-length SPIR-V instruction");
			}
			if ((instruction & 0xffffu) == 15)
			{
				if (count < 4 || word(at + 2) == 0 || word(at + 2) >= word(3))
				{
					return std::unexpected("Malformed SPIR-V entry point");
				}
				const auto begin = (at + 3) * 4;
				std::size_t end = begin;
				while (end < (at + count) * 4 && bytes[end] != std::byte{0})
				{
					++end;
				}
				if (end == (at + count) * 4)
				{
					return std::unexpected("Unterminated SPIR-V entry point name");
				}
				const std::string_view name(reinterpret_cast<const char*>(bytes.data() + begin), end - begin);
				found |= word(at + 1) == 4 && name == entry;
			}
			at += count;
		}
		return found;
	}
} // namespace aether::gpu
