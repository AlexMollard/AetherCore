#pragma once

#include <cstdint>

namespace aether
{
	struct Entity
	{
		std::uint32_t id = 0;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return id != 0;
		}

		bool operator==(const Entity&) const noexcept = default;
		bool operator!=(const Entity&) const noexcept = default;
	};
} // namespace aether
