#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/Components.hpp"

namespace aether
{
	class World;
	class ServiceContainer;
	struct Entity;
} // namespace aether

namespace aether::editor
{
	class UndoStack;

	// One component's editable fields, captured as JSON. `isReflected` selects which
	// path reads/writes them back: the reflection registry, or the hand-authored
	// ComponentFieldSet (currently just Material).
	struct ReflectedComponentFields
	{
		std::string name;
		nlohmann::json fields;
		bool isReflected = true;
	};

	// Snapshot every reflected component present on `entity`.
	//
	// The bespoke (hand-written) drawers in ComponentDrawers*.cpp mutate components
	// directly through ~90 individual widgets. Rather than give each one its own undo
	// hook, callers snapshot around the whole block and hand the result to
	// RecordReflectedFieldEdits, which attributes each change to its field.
	[[nodiscard]] std::vector<ReflectedComponentFields> CaptureReflectedFields(World& world, Entity entity, ServiceContainer& services);

	// Diff `before` against the entity's current state and feed every changed field
	// into the undo stack's coalescing buffer, so a multi-frame drag through a bespoke
	// drawer still collapses into a single command. Safe to call when some of those
	// edits were already recorded directly - RecordFieldEdit keeps the earliest before
	// and latest after per field, so recording the same change twice is idempotent.
	void RecordReflectedFieldEdits(UndoStack& undo, World& world, Entity entity, ServiceContainer& services, const std::vector<ReflectedComponentFields>& before);

	// ScriptComponent is not reflected, so the field snapshot above cannot see it.
	// These mirror the same capture/record shape for an entity's script list, covering
	// every script edit (type change, property edit, slot add/remove) at once.
	[[nodiscard]] std::vector<ScriptEntry> CaptureScripts(World& world, Entity entity);
	void RecordScriptEdits(UndoStack& undo, World& world, Entity entity, const std::vector<ScriptEntry>& before);

	// Same shape again for tag membership, which lives in a side table rather than on
	// the entity as a component.
	[[nodiscard]] std::vector<std::uint32_t> CaptureTags(World& world, Entity entity);
	void RecordTagEdits(UndoStack& undo, World& world, Entity entity, const std::vector<std::uint32_t>& before);

	// Draws every reflected component not named in `exclude` (those have bespoke
	// drawers). `services` supplies the UndoStack, so field edits and the section's
	// remove button record typed commands instead of relying on a scene diff.
	void DrawReflectedComponents(World& world, Entity entity, ServiceContainer& services, std::initializer_list<std::string_view> exclude);
} // namespace aether::editor
