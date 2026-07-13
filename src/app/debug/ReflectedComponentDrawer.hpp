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
	// Draws an inspector section for every reflected component (see
	// scene/reflection/) present on the entity EXCEPT those named in `exclude`,
	// using generic widgets chosen by each field's FieldType. This is the inspector
	// consumer of the reflection registry: it replaces the hand-written drawers for
	// every component whose UI needs nothing bespoke. The excluded ones keep a
	// hand-written drawer because they do real per-component work the generic drawer
	// can't (physics body/joint rebuilds, the camera main-view tag, skinned
	// group-drive, material textures, Transform axis chips) or are drawn elsewhere
	// (the entity's name field).
	void DrawReflectedComponents(World& world, Entity entity, std::initializer_list<std::string_view> exclude);
} // namespace aether::editor
