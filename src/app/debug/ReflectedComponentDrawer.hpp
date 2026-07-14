#pragma once

#include <initializer_list>
#include <string_view>

namespace aether
{
	class World;
	struct Entity;
} // namespace aether

namespace aether::editor
{
	void DrawReflectedComponents(World& world, Entity entity, std::initializer_list<std::string_view> exclude);
}
