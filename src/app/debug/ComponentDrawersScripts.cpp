#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
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
#include "debug/EditorCommand.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/UndoStack.hpp"
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

	void AssignScriptType(ScriptEntry& script, std::string typeName)
	{
		if (script.path == typeName)
		{
			return;
		}
		script.path = std::move(typeName);
		script.attached = false;
		script.properties.clear();
	}

	ScriptEntry& AddScriptSlot(ScriptComponent& sc, std::string typeName = {})
	{
		ScriptEntry& script = sc.scripts.emplace_back();
		if (!typeName.empty())
		{
			AssignScriptType(script, std::move(typeName));
		}
		return script;
	}

	bool AcceptScriptDrop(ScriptComponent& sc)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return false;
		}

		bool assigned = false;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
		{
			if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
			{
				const auto* script = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
				AddScriptSlot(sc, script->typeName);
				assigned = true;
			}
		}
		ImGui::EndDragDropTarget();
		return assigned;
	}

	void DrawScriptEntry(app::LayerContext& context, World& world, Entity entity, ScriptComponent& sc, ScriptEntry& script, std::size_t scriptIndex)
	{
		ImGui::PushID(static_cast<int>(scriptIndex));
		auto* cs = context.TryGet<app::scripting::CSharpScriptingSubsystem>();
		const bool scriptingAvailable = cs != nullptr && cs->IsAvailable();
		const char* preview = script.path.empty() ? "Drop or choose a script" : script.path.c_str();
		ImGui::SetNextItemWidth(-34.0f);
		if (ImGui::BeginCombo("Script Type", preview))
		{
			if (ImGui::Selectable("None", script.path.empty()))
			{
				AssignScriptType(script, {});
			}
			if (cs != nullptr)
			{
				for (const std::string& typeName: cs->GetScriptTypeNames())
				{
					const bool selected = script.path == typeName;
					if (ImGui::Selectable(typeName.c_str(), selected))
					{
						AssignScriptType(script, typeName);
					}
					if (selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
			{
				if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
				{
					const auto* dropped = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
					AssignScriptType(script, dropped->typeName);
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine();
		const bool removeScript = ImGui::SmallButton(ICON_FA_XMARK);
		ImGui::SetItemTooltip("Remove this script");
		if (removeScript)
		{
			sc.scripts.erase(sc.scripts.begin() + static_cast<std::ptrdiff_t>(scriptIndex));
			ImGui::PopID();
			return;
		}

		if (script.path.empty())
		{
			ImGui::TextDisabled("Drag a C# script here from File Explorer.");
			ImGui::PopID();
			return;
		}

		ImGui::TextDisabled(script.attached ? "Attached (running while playing)" : "Attaches on the next Play tick");
		if (script.attached && ImGui::SmallButton("Re-attach"))
		{
			script.attached = false;
		}

		if (!scriptingAvailable)
		{
			ImGui::PopID();
			return;
		}
		const auto props = cs->GetScriptProperties(script.path);
		if (props.empty())
		{
			ImGui::PopID();
			return;
		}

		std::uint64_t handle = 0;
		if (auto* runner = context.TryGet<app::ScriptComponentSystem>())
		{
			handle = runner->GetInstanceHandle(entity.id, static_cast<std::uint32_t>(scriptIndex));
		}

		ImGui::SeparatorText("Properties");
		std::vector<int> propertyOrder;
		propertyOrder.reserve(props.size());
		for (int i = 0; i < static_cast<int>(props.size()); ++i)
		{
			if (props[i].name == "Self" && props[i].type == ScriptPropertyValue::Type::Entity)
			{
				propertyOrder.insert(propertyOrder.begin(), i);
			}
			else
			{
				propertyOrder.push_back(i);
			}
		}
		for (const int i: propertyOrder)
		{
			const auto& info = props[i];
			const bool isSelfProperty = info.name == "Self" && info.type == ScriptPropertyValue::Type::Entity;
			ScriptPropertyValue value;
			value.type = info.type;
			bool haveValue = false;
			if (handle != 0 && cs->GetPropertyValue(handle, i, value))
			{
				haveValue = true;
			}
			else if (const auto it = script.properties.find(info.name); it != script.properties.end())
			{
				value = it->second;
				haveValue = true;
			}
			else if (isSelfProperty)
			{
				value.i64 = entity.id;
				haveValue = true;
			}
			else if (cs->GetDefaultPropertyValue(script.path, i, value))
			{
				haveValue = true;
			}
			if (!haveValue)
			{
				continue;
			}

			bool edited = false;
			switch (info.type)
			{
				case ScriptPropertyValue::Type::Float:
				{
					float f = value.f4[0];
					if (PropFloat(info.name.c_str(), &f, 0.1f))
					{
						value.f4[0] = f;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Int:
				case ScriptPropertyValue::Type::Enum:
				{
					int n = static_cast<int>(value.i64);
					if (PropInt(info.name.c_str(), &n))
					{
						value.i64 = n;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Bool:
				{
					bool b = value.i64 != 0;
					if (PropCheckbox(info.name.c_str(), &b))
					{
						value.i64 = b ? 1 : 0;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Vector3:
				{
					glm::vec3 v{value.f4[0], value.f4[1], value.f4[2]};
					if (iw::Vec3Row(info.name.c_str(), v, 0.0f, 0.1f))
					{
						value.f4[0] = v.x;
						value.f4[1] = v.y;
						value.f4[2] = v.z;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::String:
				{
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", value.str.c_str());
					if (PropInputText(info.name.c_str(), buf, sizeof(buf)))
					{
						value.str = buf;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Entity:
				{
					const Entity target{static_cast<std::uint32_t>(value.i64)};
					const bool targetAlive = target.IsValid() && world.GetRegistry().valid(World::ToEntt(target));
					const std::string label = targetAlive ? std::string(EntityDisplayName(world, target)) + " #" + std::to_string(target.id) : "None";
					const std::string buttonId = label + "##entityField" + info.name;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(info.name.c_str());
					ImGui::SameLine(iw::LabelWidth());
					const ImGuiStyle& style = ImGui::GetStyle();
					const float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + style.FramePadding.x * 2.0f;
					const float selfButtonWidth = ImGui::CalcTextSize(ICON_FA_LINK).x + style.FramePadding.x * 2.0f;
					const float trailingButtonWidth = clearButtonWidth + style.ItemSpacing.x + (isSelfProperty ? selfButtonWidth + style.ItemSpacing.x : 0.0f);
					const float entityButtonWidth = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x - trailingButtonWidth);
					ImGui::Button(buttonId.c_str(), ImVec2(entityButtonWidth, 0.0f));
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload))
						{
							if (payload->DataSize == sizeof(std::uint32_t))
							{
								value.i64 = *static_cast<const std::uint32_t*>(payload->Data);
								edited = true;
							}
							else if (payload->DataSize == sizeof(dragdrop::EntityPayload))
							{
								value.i64 = static_cast<const dragdrop::EntityPayload*>(payload->Data)->id;
								edited = true;
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (isSelfProperty)
					{
						if (ImGui::SmallButton((std::string(ICON_FA_LINK "##useSelf") + info.name).c_str()))
						{
							value.i64 = entity.id;
							edited = true;
						}
						ImGui::SetItemTooltip("Set Self to this entity");
						ImGui::SameLine();
					}
					if (ImGui::SmallButton((std::string(ICON_FA_XMARK "##clear") + info.name).c_str()))
					{
						value.i64 = 0;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Component:
				{
					// that carry the required component (value.str = its catalog
					const std::string& componentName = value.str;
					const editor::ComponentCatalogEntry* catEntry = editor::FindComponent(componentName);
					const Entity target{static_cast<std::uint32_t>(value.i64)};
					const bool targetAlive = target.IsValid() && world.GetRegistry().valid(World::ToEntt(target));
					const std::string prefix = catEntry != nullptr ? catEntry->icon + "  " : std::string();
					const std::string label = targetAlive ? prefix + std::string(EntityDisplayName(world, target)) + " #" + std::to_string(target.id) : "None";
					const std::string buttonId = label + "##compField" + info.name;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(info.name.c_str());
					if (!componentName.empty())
					{
						ImGui::SetItemTooltip("Link an entity that has a '%s' component", componentName.c_str());
					}
					ImGui::SameLine(iw::LabelWidth());
					const ImGuiStyle& style = ImGui::GetStyle();
					const float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + style.FramePadding.x * 2.0f;
					const float trailingButtonWidth = clearButtonWidth + style.ItemSpacing.x;
					const float entityButtonWidth = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x - trailingButtonWidth);
					ImGui::Button(buttonId.c_str(), ImVec2(entityButtonWidth, 0.0f));
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload))
						{
							std::uint32_t droppedId = 0;
							if (payload->DataSize == sizeof(std::uint32_t))
							{
								droppedId = *static_cast<const std::uint32_t*>(payload->Data);
							}
							else if (payload->DataSize == sizeof(dragdrop::EntityPayload))
							{
								droppedId = static_cast<const dragdrop::EntityPayload*>(payload->Data)->id;
							}
							const Entity dropped{droppedId};
							if (dropped.IsValid() && catEntry != nullptr && catEntry->has(world, dropped))
							{
								value.i64 = droppedId;
								edited = true;
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (ImGui::SmallButton((std::string(ICON_FA_XMARK "##clearComp") + info.name).c_str()))
					{
						value.i64 = 0;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::None:
				default:
					break;
			}

			if (edited)
			{
				value.type = info.type;
				script.properties[info.name] = value;
				if (handle != 0)
				{
					cs->SetPropertyValue(handle, i, value);
				}
			}
		}
		ImGui::PopID();
	}

	void AddScriptToEntity(World& world, Entity entity, std::string typeName)
	{
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			sc = &world.Emplace<ScriptComponent>(entity);
		}
		AddScriptSlot(*sc, std::move(typeName));
	}

	bool AcceptScriptDropOnEntity(World& world, Entity entity)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return false;
		}

		bool accepted = false;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
		{
			if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
			{
				const auto* script = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
				AddScriptToEntity(world, entity, script->typeName);
				accepted = true;
			}
		}
		ImGui::EndDragDropTarget();
		return accepted;
	}

	void DrawScript(app::LayerContext& context, World& world, Entity entity)
	{
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_CODE "  Script", ICON_FA_XMARK "##removeScript", removed, ImGuiTreeNodeFlags_DefaultOpen);
		if (removed)
		{
			// One command per script, so undo brings each back. Removing the component whole
			// recorded nothing at all, so Ctrl+Z reached past it into an unrelated edit and
			// every script the entity had was gone for good.
			if (auto* undo = context.services.TryGet<UndoStack>())
			{
				if (const auto* scripts = world.TryGet<ScriptComponent>(entity))
				{
					for (const ScriptEntry& entry: scripts->scripts)
					{
						undo->Record(std::make_unique<RemoveScriptCommand>(entity.id, entry.path, entry));
					}
				}
			}
			world.Remove<ScriptComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}

		if (ImGui::Button(ICON_FA_PLUS "  Add Script"))
		{
			AddScriptSlot(*sc);
		}
		AcceptScriptDrop(*sc);
		if (sc->scripts.empty())
		{
			ImGui::TextDisabled("Drag a C# script here from File Explorer.");
			return;
		}

		for (std::size_t i = 0; i < sc->scripts.size();)
		{
			ImGui::Separator();
			const std::size_t before = sc->scripts.size();
			DrawScriptEntry(context, world, entity, *sc, sc->scripts[i], i);
			if (sc->scripts.size() == before)
			{
				++i;
			}
		}
	}

} // namespace aether::editor
