#include "WebScene.hpp"

#include <algorithm>
#include "io/FileSystem.hpp"
#include "gpu/WebMaterialShader.hpp"
#include "scene/Hierarchy.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDrawBuilder.hpp"

namespace aether::app::web
{
	Expected<void> WebScene::ExtractUi(RenderFramePacket& packet, unsigned width, unsigned height)
	{
		if (!width || !height)
		{
			return {};
		}
		const auto loadFont = [&](std::string_view name) -> Expected<void>
		{
			if (!m_fonts.Load(name))
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_FONT: missing font curves for " + std::string(name)));
			}
			auto* font = m_fonts.GetMutable(name);
			if (!font || !m_graphics.UploadUiFont(*font))
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_FONT: upload failed for " + std::string(name)));
			}
			return {};
		};
		for (const auto& [entity, text]: world.View<ui::UIText>().each())
		{
			AE_TRY_VOID(loadFont(text.fontName));
		}
		for (const auto& [entity, button]: world.View<ui::UIButton>().each())
		{
			AE_TRY_VOID(loadFont(button.fontName));
		}
		for (const auto& [entity, box]: world.View<ui::UITextBox>().each())
		{
			AE_TRY_VOID(loadFont(box.fontName));
		}
		// UI textures share the color-managed registry, not the legacy unorm sprite registry:
		// the native UI fragment expects sRGB decoding before its explicit display re-encode.
		m_sink.colorManaged = true;
		ui::BuildDrawCommands(world, packet.renderUi.commands, packet.renderUi.materials, &m_fonts, &m_model_textures);
		m_sink.colorManaged = false;
		for (const auto& [entity, effect, rect]: world.View<ui::UIEffect, ui::UIRect>().each())
		{
			if (!ecs::HasDisabledAncestor(world, World::FromEntt(entity)))
			{
				packet.renderUi.effects.push_back({effect.shader, rect.resolvedRect, effect.params, effect.color0, effect.color1, effect.background, effect.sortOrder});
			}
		}
		std::stable_sort(packet.renderUi.effects.begin(), packet.renderUi.effects.end(), [](const auto& a, const auto& b) { return a.sortOrder < b.sortOrder; });
		const auto shader = [&](std::string_view name, bool effect, bool glyph) -> Expected<void>
		{
			if (name.empty() || name.find_first_of("\\:") != std::string_view::npos || name.find('\0') != std::string_view::npos)
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_SHADER: invalid shader name"));
			}
			for (std::size_t start = 0; start <= name.size();)
			{
				const auto end = name.find('/', start);
				const auto part = name.substr(start, end == std::string_view::npos ? end : end - start);
				if (part.empty() || part == "." || part == "..")
				{
					AE_UNEXPECTED(AetherError::Asset("WEB_UI_SHADER: invalid shader path"));
				}
				if (end == std::string_view::npos)
				{
					break;
				}
				start = end + 1;
			}
			if (m_graphics.HasUiShader(name, effect, glyph))
			{
				return {};
			}
			const auto source = io::FileSystem::ReadFile("project://assets/shaders/" + std::string(name) + (effect ? ".ui-effect.wshader" : glyph ? ".ui-material-glyph.wshader" : ".ui-material.wshader"));
			if (!source)
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_SHADER: missing cooked original source for " + std::string(name)));
			}
			const auto decoded = gpu::DecodeWebMaterialShader(*source);
			if (!decoded)
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_SHADER: " + decoded.error()));
			}
			if (!m_graphics.UploadUiShader(name, effect, glyph, *decoded))
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_UI_SHADER: pipeline creation failed for " + std::string(name)));
			}
			return {};
		};
		for (const auto& effect: packet.renderUi.effects)
		{
			AE_TRY_VOID(shader(effect.shader, true, false));
		}
		for (const auto& command: packet.renderUi.commands)
		{
			const auto id = ui::UiFlagsShaderId(command.flags);
			if (id && id <= packet.renderUi.materials.size())
			{
				AE_TRY_VOID(shader(packet.renderUi.materials[id - 1].shader, false, command.type == ui::kShapeGlyph));
			}
		}
		return {};
	}
} // namespace aether::app::web
