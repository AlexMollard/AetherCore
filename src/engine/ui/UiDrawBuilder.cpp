#include "ui/UiDrawBuilder.hpp"

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "material/TextureRegistry.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	// Emits one UiDrawCommand for `img`, using `rect`'s already-resolved pixel
	// rect. Invalid texture -> solid fill (rounded when cornerRadius > 0); valid
	// texture -> full-UV textured rect with its live bindless slot when the
	// renderer supplies the texture registry.
	static void EmitImage(const UIRect& rect, const UIImage& img, const TextureRegistry* textures, int layer, std::vector<UiDrawCommand>& out)
	{
		UiDrawCommand cmd;
		cmd.data0 = rect.resolvedRect; // x,y,w,h
		cmd.color = img.color;
		cmd.layer = layer;
		if (img.texture.IsValid())
		{
			cmd.type = kShapeTexturedRect;
			cmd.data1 = {0.f, 0.f, 1.f, 1.f}; // full UVs
			cmd.textureSlot = textures != nullptr ? textures->ResolveSlot(img.texture) : 0xFFFFFFFFu;
		}
		else
		{
			cmd.type = kShapeRect;
			cmd.data1.x = img.cornerRadius;
		}
		out.push_back(cmd);
	}

	static void EmitText(const UIRect& rect, const UIText& text, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const FontAsset* font = fonts.Load(text.fontName);
		if (font == nullptr)
		{
			return;
		}
		if (font->atlasBindlessSlot == 0xFFFFFFFFu)
		{
			return;
		}

		const std::vector<ShapedGlyph> glyphs = ShapeText(*font, text.text, text.pixelSize, rect.resolvedRect, text.wrap, static_cast<int>(text.hAlign), static_cast<int>(text.vAlign));
		for (const ShapedGlyph& glyph: glyphs)
		{
			UiDrawCommand cmd;
			cmd.data0 = glyph.rect;
			cmd.data1 = glyph.uv;
			cmd.color = text.color;
			cmd.type = kShapeSdfGlyph;
			cmd.layer = layer++;
			cmd.textureSlot = font->atlasBindlessSlot;
			out.push_back(cmd);
		}
	}

	// Pre-order walk of the HierarchyComponent subtree rooted at `entity`: emits
	// a command for `entity` itself (if it carries both UIRect and UIImage),
	// then recurses into its children. `layer` is the running emission index
	// shared across the whole canvas, so it must be threaded through by reference.
	static void Walk(World& world, Entity entity, FontRegistry* fonts, const TextureRegistry* textures, int& layer, std::vector<UiDrawCommand>& out)
	{
		if (const auto* rect = world.TryGet<UIRect>(entity))
		{
			if (const auto* img = world.TryGet<UIImage>(entity))
			{
				EmitImage(*rect, *img, textures, layer++, out);
			}
			if (fonts != nullptr)
			{
				if (const auto* text = world.TryGet<UIText>(entity))
				{
					EmitText(*rect, *text, *fonts, layer, out);
				}
			}
		}

		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
		{
			for (const Entity child: hierarchy->children)
			{
				Walk(world, child, fonts, textures, layer, out);
			}
		}
	}

	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out, FontRegistry* fonts, const TextureRegistry* textures)
	{
		out.clear();
		int layer = 0;
		world.View<UICanvas>().each([&](entt::entity canvasEntity, UICanvas&) { Walk(world, World::FromEntt(canvasEntity), fonts, textures, layer, out); });
	}
} // namespace aether::ui
