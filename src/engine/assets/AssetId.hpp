#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

namespace aether
{
	struct AssetId
	{
		std::uint64_t value = 0;

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return value != 0;
		}

		[[nodiscard]] std::string ToHex() const
		{
			char buf[17];
			std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
			return std::string(buf, 16);
		}

		[[nodiscard]] static AssetId FromHex(std::string_view hex)
		{
			if (hex.empty() || hex.size() > 16)
			{
				return AssetId{};
			}
			std::uint64_t v = 0;
			for (const char c: hex)
			{
				std::uint64_t digit = 0;
				if (c >= '0' && c <= '9')
				{
					digit = static_cast<std::uint64_t>(c - '0');
				}
				else if (c >= 'a' && c <= 'f')
				{
					digit = static_cast<std::uint64_t>(c - 'a') + 10u;
				}
				else if (c >= 'A' && c <= 'F')
				{
					digit = static_cast<std::uint64_t>(c - 'A') + 10u;
				}
				else
				{
					return AssetId{};
				}
				v = (v << 4) | digit;
			}
			return AssetId{v};
		}

		friend constexpr bool operator==(AssetId a, AssetId b) noexcept
		{
			return a.value == b.value;
		}

		friend constexpr bool operator!=(AssetId a, AssetId b) noexcept
		{
			return a.value != b.value;
		}
	};
} // namespace aether

template<>
struct std::hash<aether::AssetId>
{
	std::size_t operator()(const aether::AssetId& id) const noexcept
	{
		return static_cast<std::size_t>(id.value);
	}
};
