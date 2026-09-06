#include "scripting/interop/InteropCommon.hpp"

#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// MeshRenderer.SetVisible/GetVisible's only native entry points. This is the toggle
// Ragdoll.DriveSkin's own doc comment names for choosing between a ragdoll's driven
// skin and its capsule/box bones: both keep rendering by default (see
// RagdollBuilder.cpp - a bone's MeshRendererComponent is never touched by
// SpawnRagdoll or DriveSkin), so a caller that wants only one visible flips this on
// whichever entity it wants hidden. A dead entity, or one with no MeshRenderer,
// answers false/no-op rather than fabricating a component - a mesh with no renderer
// is already invisible.
AE_SCRIPT_API std::int32_t aether_mesh_renderer_get_visible(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	if (!EntityAlive(id))
	{
		return 0;
	}
	const auto* mr = ActiveWorld().TryGet<aether::MeshRendererComponent>(aether::Entity{id});
	return (mr != nullptr && mr->visible) ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_mesh_renderer_set_visible(std::uint32_t id, std::int32_t visible)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(id))
	{
		return;
	}
	auto* mr = ActiveWorld().TryGet<aether::MeshRendererComponent>(aether::Entity{id});
	if (mr != nullptr)
	{
		mr->visible = visible != 0;
	}
	});
}
