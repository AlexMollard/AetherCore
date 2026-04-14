#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace aether
{
	// Opaque handle to an entity in a World.
	// id == 0 is the null / invalid entity.
	struct Entity
	{
		std::uint32_t id = 0;

		[[nodiscard]] bool IsValid() const noexcept { return id != 0; }

		bool operator==(const Entity&) const noexcept = default;
		bool operator!=(const Entity&) const noexcept = default;
	};
}

template <>
struct std::hash<aether::Entity>
{
	std::size_t operator()(const aether::Entity& e) const noexcept
	{
		return std::hash<std::uint32_t>{}(e.id);
	}
};
