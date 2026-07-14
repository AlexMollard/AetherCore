#pragma once

#include <string>

#include <glm/glm.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::editor
{
	//   2. spawns it with the correct layout - a lone static mesh sits directly on
	Entity ImportModelIntoScene(World& world, ServiceContainer& services, const std::string& vfsModelPath, const glm::mat4& localToWorld, const std::string& name, std::string& error);
} // namespace aether::editor
