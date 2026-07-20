#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/AssetTypes.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/EditorDragDrop.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "debug/Icons.hpp"
#include "utils/Logger.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/SpriteAuthoringUi.hpp"
#include "layers/AppLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "physics2d/SpriteColliderGen.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::editor
{
	using iw::AccentButton;
	using iw::PropCheckbox;
	using iw::PropColor3;
	using iw::PropColor4;
	using iw::PropCombo;
	using iw::PropComboStr;
	using iw::PropDrag2;
	using iw::PropFloat;
	using iw::PropInputText;
	using iw::PropInt;
	using iw::PropSlider;
	using iw::PropText;
	using iw::RemovableSection;
	using iw::SectionHeader;
	namespace
	{
		void ApplyAnchorPreset(ui::UIRect& rect, int preset)
		{
			if (preset == 9)
			{
				rect.anchorMin = {0.f, 0.f};
				rect.anchorMax = {1.f, 1.f};
				rect.offsetMin = {0.f, 0.f};
				rect.offsetMax = {0.f, 0.f};
				return;
			}

			const int xIndex = preset % 3;
			const int yIndex = preset / 3;
			const glm::vec2 anchor{static_cast<float>(xIndex) * 0.5f, static_cast<float>(yIndex) * 0.5f};
			glm::vec2 size = rect.resolvedRect.z > 0.f && rect.resolvedRect.w > 0.f ? glm::vec2(rect.resolvedRect.z, rect.resolvedRect.w) : glm::abs(rect.offsetMax - rect.offsetMin);
			size = glm::max(size, glm::vec2(1.f));

			rect.anchorMin = anchor;
			rect.anchorMax = anchor;
			rect.offsetMin = -rect.pivot * size;
			rect.offsetMax = (glm::vec2(1.f) - rect.pivot) * size;
		}
	} // namespace
	void DrawUiCanvas(World& world, Entity entity)
	{
		auto* canvas = world.TryGet<ui::UICanvas>(entity);
		if (canvas == nullptr || !SectionHeader(ICON_FA_IMAGE "  UI Canvas", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		int scaleMode = static_cast<int>(canvas->scaleMode);
		if (PropComboStr("Scale mode", &scaleMode, "Constant Pixel\0Scale With Reference\0"))
		{
			canvas->scaleMode = static_cast<ui::UICanvas::ScaleMode>(std::clamp(scaleMode, 0, 1));
		}
		PropDrag2("Reference res", &canvas->referenceResolution.x, 1.f, 1.f, 16384.f, "%.0f");
		canvas->referenceResolution = glm::max(canvas->referenceResolution, glm::vec2(1.f));
		PropInt("Sort bias", &canvas->sortBias, 1.f, -100000, 100000);
	}

	void DrawUiRect(World& world, Entity entity)
	{
		auto* rect = world.TryGet<ui::UIRect>(entity);
		if (rect == nullptr || !SectionHeader(ICON_FA_EXPAND "  UI Rect", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		static constexpr const char* kPresets[] = {
		        "Top-Left",
		        "Top-Center",
		        "Top-Right",
		        "Middle-Left",
		        "Center",
		        "Middle-Right",
		        "Bottom-Left",
		        "Bottom-Center",
		        "Bottom-Right",
		        "Stretch-All",
		};

		int preset = -1;
		if (PropCombo("Anchor preset", &preset, kPresets, IM_ARRAYSIZE(kPresets)) && preset >= 0)
		{
			ApplyAnchorPreset(*rect, preset);
		}

		PropDrag2("Anchor min", &rect->anchorMin.x, 0.01f, 0.f, 1.f, "%.2f");
		PropDrag2("Anchor max", &rect->anchorMax.x, 0.01f, 0.f, 1.f, "%.2f");
		rect->anchorMin = glm::clamp(rect->anchorMin, glm::vec2(0.f), glm::vec2(1.f));
		rect->anchorMax = glm::clamp(rect->anchorMax, glm::vec2(0.f), glm::vec2(1.f));
		rect->anchorMax = glm::max(rect->anchorMax, rect->anchorMin);

		PropDrag2("Offset min", &rect->offsetMin.x, 1.f, 0.f, 0.f, "%.0f");
		PropDrag2("Offset max", &rect->offsetMax.x, 1.f, 0.f, 0.f, "%.0f");
		if (rect->anchorMin == rect->anchorMax)
		{
			rect->offsetMax = glm::max(rect->offsetMax, rect->offsetMin + glm::vec2(1.f));
		}
		PropDrag2("Pivot", &rect->pivot.x, 0.01f, 0.f, 1.f, "%.2f");
		rect->pivot = glm::clamp(rect->pivot, glm::vec2(0.f), glm::vec2(1.f));
		ImGui::TextDisabled("Resolved %.1f, %.1f  %.1f x %.1f", rect->resolvedRect.x, rect->resolvedRect.y, rect->resolvedRect.z, rect->resolvedRect.w);
	}

	void DrawUiImage(World& world, Entity entity)
	{
		auto* image = world.TryGet<ui::UIImage>(entity);
		if (image == nullptr || !SectionHeader(ICON_FA_IMAGE "  UI Image", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		PropColor4("Color", &image->color.x);
		PropFloat("Corner radius", &image->cornerRadius, 0.5f, 0.f, 200.f, "%.1f");
		PropCheckbox("Pixel art", &image->pixelArt);
		if (image->texture.IsValid())
		{
			PropText("Texture", "entry %u", image->texture.index);
		}
		else
		{
			PropText("Texture", "(none)");
		}
	}

	void DrawUiText(World& world, Entity entity)
	{
		auto* text = world.TryGet<ui::UIText>(entity);
		if (text == nullptr || !SectionHeader(ICON_FA_CODE "  UI Text", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		char textBuf[512]{};
		std::snprintf(textBuf, sizeof(textBuf), "%s", text->text.c_str());
		iw::PropLabel("Text");
		if (ImGui::InputTextMultiline("##text", textBuf, sizeof(textBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f)))
		{
			text->text = textBuf;
		}

		char fontBuf[128]{};
		std::snprintf(fontBuf, sizeof(fontBuf), "%s", text->fontName.c_str());
		if (PropInputText("Font", fontBuf, sizeof(fontBuf), "default"))
		{
			text->fontName = fontBuf;
		}

		PropFloat("Pixel size", &text->pixelSize, 0.5f, 4.f, 200.f, "%.0f");
		text->pixelSize = std::max(text->pixelSize, 1.f);
		PropColor4("Color", &text->color.x);

		int hAlign = static_cast<int>(text->hAlign);
		if (PropComboStr("H align", &hAlign, "Left\0Center\0Right\0"))
		{
			text->hAlign = static_cast<ui::UIText::HAlign>(std::clamp(hAlign, 0, 2));
		}
		int vAlign = static_cast<int>(text->vAlign);
		if (PropComboStr("V align", &vAlign, "Top\0Middle\0Bottom\0"))
		{
			text->vAlign = static_cast<ui::UIText::VAlign>(std::clamp(vAlign, 0, 2));
		}
		PropCheckbox("Wrap", &text->wrap);
	}
} // namespace aether::editor
