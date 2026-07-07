#include "ui/UiDrawBuilder.hpp"

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	// Emits one UiDrawCommand for `img`, using `rect`'s already-resolved pixel
	// rect. Invalid texture -> solid fill (rounded when cornerRadius > 0); valid
	// texture -> full-UV textured rect (the bindless slot is resolved later, at
	// GPU upload time).
	static void EmitImage(const UIRect& rect, const UIImage& img, int layer, std::vector<UiDrawCommand>& out)
	{
		UiDrawCommand cmd;
		cmd.data0 = rect.resolvedRect; // x,y,w,h
		cmd.color = img.color;
		cmd.layer = layer;
		if (img.texture.IsValid())
		{
			cmd.type = kShapeTexturedRect;
			cmd.data1 = {0.f, 0.f, 1.f, 1.f}; // full UVs
			cmd.textureSlot = 0;              // resolved by the GPU renderer at upload (later task)
		}
		else
		{
			cmd.type = kShapeRect;
			cmd.data1.x = img.cornerRadius;
		}
		out.push_back(cmd);
	}

	// Pre-order walk of the HierarchyComponent subtree rooted at `entity`: emits
	// a command for `entity` itself (if it carries both UIRect and UIImage),
	// then recurses into its children. `layer` is the running emission index
	// shared across the whole canvas, so it must be threaded through by reference.
	static void Walk(World& world, Entity entity, int& layer, std::vector<UiDrawCommand>& out)
	{
		if (const auto* rect = world.TryGet<UIRect>(entity))
		{
			if (const auto* img = world.TryGet<UIImage>(entity))
			{
				EmitImage(*rect, *img, layer++, out);
			}
		}

		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
		{
			for (const Entity child: hierarchy->children)
			{
				Walk(world, child, layer, out);
			}
		}
	}

	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out)
	{
		out.clear();
		int layer = 0;
		world.View<UICanvas>().each([&](entt::entity canvasEntity, UICanvas&) { Walk(world, World::FromEntt(canvasEntity), layer, out); });
	}
} // namespace aether::ui
