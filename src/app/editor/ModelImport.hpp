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
	// One-call "add a model to the scene" used by both the editor UI (File Explorer
	// / hierarchy) and the MCP control server, so every entry point behaves the same:
	//   1. bakes the raw glTF if it hasn't been imported yet (editor-only),
	//   2. spawns it with the correct layout - a lone static mesh sits directly on
	//      one entity, while a multi-primitive / skinned model becomes a root
	//      container with one child per primitive (each carrying its own mesh index,
	//      material and skin), and
	//   3. registers its meshes/materials so the asset pickers list them.
	//
	// Editor-only: it reaches the asset pipeline through EnsureModelBaked, which the
	// shipped runtime never links. Returns the spawned root entity, or an invalid
	// Entity on failure with `error` set. `name` overrides the entity name when
	// non-empty (otherwise the model's file stem is used).
	Entity ImportModelIntoScene(World& world, ServiceContainer& services, const std::string& vfsModelPath, const glm::mat4& localToWorld, const std::string& name, std::string& error);
} // namespace aether::editor
