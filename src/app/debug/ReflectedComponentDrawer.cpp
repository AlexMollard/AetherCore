#include "debug/ReflectedComponentDrawer.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "debug/EditorCommand.hpp"
#include "debug/Icons.hpp"
#include "Color.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/UndoStack.hpp"
#include "editor/ComponentFields.hpp"
#include "editor/ReflectionJson.hpp"
#include "scene/Entity.hpp"
#include "scene/TagSlots.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	namespace
	{
		using reflect::FieldType;

		bool DrawScalarField(const char* lbl, reflect::FieldType type, const reflect::FieldMeta& meta, reflect::FieldValue& v)
		{
			const float speed = meta.speed > 0.0f ? meta.speed : 0.05f;
			bool changed = false;
			switch (type)
			{
				case FieldType::Float:
				{
					float x = static_cast<float>(v.num);
					if (iw::PropFloat(lbl, &x, speed, meta.min, meta.max))
					{
						v.num = x;
						changed = true;
					}
					break;
				}
				case FieldType::Int:
				{
					int x = static_cast<int>(v.num);
					if (iw::PropInt(lbl, &x))
					{
						v.num = x;
						changed = true;
					}
					break;
				}
				case FieldType::UInt:
				{
					int x = static_cast<int>(v.num);
					if (iw::PropInt(lbl, &x, 1.0f, 0, 0))
					{
						v.num = x < 0 ? 0 : x;
						changed = true;
					}
					break;
				}
				case FieldType::Bool:
				{
					bool b = v.boolean;
					if (iw::PropCheckbox(lbl, &b))
					{
						v.boolean = b;
						changed = true;
					}
					break;
				}
				case FieldType::Vec2:
				{
					glm::vec2 vv = glm::vec2(v.vec);
					if (iw::PropDrag2(lbl, &vv.x, speed))
					{
						v.vec = glm::vec4(vv, 0.0f, 0.0f);
						changed = true;
					}
					break;
				}
				case FieldType::Vec3:
				{
					glm::vec3 vv = glm::vec3(v.vec);
					if (iw::Vec3Row(lbl, vv, 0.0f, speed))
					{
						v.vec = glm::vec4(vv, 0.0f);
						changed = true;
					}
					break;
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					glm::vec4 c = v.vec;
					if (iw::PropColor4(lbl, &c.x))
					{
						v.vec = c;
						changed = true;
					}
					break;
				}
				case FieldType::Color3:
				{
					glm::vec3 c = glm::vec3(v.vec);
					if (iw::PropColor3(lbl, &c.x))
					{
						v.vec = glm::vec4(c, 0.0f);
						changed = true;
					}
					break;
				}
				case FieldType::Enum:
				{
					if (meta.enumTable != nullptr)
					{
						std::vector<const char*> items;
						int cur = 0;
						for (std::size_t i = 0; i < meta.enumTable->values.size(); ++i)
						{
							items.push_back(meta.enumTable->values[i].first.c_str());
							if (meta.enumTable->values[i].second == v.enumValue)
							{
								cur = static_cast<int>(i);
							}
						}
						if (iw::PropCombo(lbl, &cur, items.data(), static_cast<int>(items.size())) && cur >= 0 && cur < static_cast<int>(meta.enumTable->values.size()))
						{
							v.enumValue = meta.enumTable->values[static_cast<std::size_t>(cur)].second;
							changed = true;
						}
					}
					break;
				}
				case FieldType::String:
				{
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", v.str.c_str());
					if (iw::PropInputText(lbl, buf, sizeof(buf)))
					{
						v.str = buf;
						changed = true;
					}
					break;
				}
				case FieldType::EntityRef:
					iw::PropText(lbl, "entity %llu", static_cast<unsigned long long>(v.entity));
					break;
				case FieldType::List:
					break; // nested lists are not modelled; the top level is drawn in DrawField
			}
			return changed;
		}

		// Editable rows for a FieldType::List: each row draws its element sub-fields, with
		// per-row remove and a trailing add. Any edit rebuilds and re-sets the whole list
		// (the component setter re-applies any validation - sorting, clamping, defaults).
		bool DrawListField(const reflect::FieldDesc& f, reflect::FieldValue& v)
		{
			bool changed = false;
			ImGui::PushID(f.name.c_str());
			if (ImGui::TreeNodeEx(f.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
			{
				for (std::size_t r = 0; r < v.list.size();)
				{
					ImGui::PushID(static_cast<int>(r));
					std::vector<reflect::FieldValue>& row = v.list[r];
					for (std::size_t i = 0; i < f.elementFields.size() && i < row.size(); ++i)
					{
						changed |= DrawScalarField(f.elementFields[i].name.c_str(), f.elementFields[i].type, reflect::FieldMeta{}, row[i]);
					}
					const bool removed = ImGui::SmallButton("Remove");
					ImGui::Separator();
					ImGui::PopID();
					if (removed)
					{
						v.list.erase(v.list.begin() + static_cast<std::ptrdiff_t>(r));
						changed = true;
						continue;
					}
					++r;
				}
				if (ImGui::SmallButton("Add"))
				{
					std::vector<reflect::FieldValue> row;
					row.reserve(f.elementFields.size());
					for (const reflect::ListElementDesc& ed: f.elementFields)
					{
						reflect::FieldValue ev;
						ev.type = ed.type;
						row.push_back(std::move(ev));
					}
					v.list.push_back(std::move(row));
					changed = true;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
			return changed;
		}

		// Routes one field's before/after into the undo stack, which coalesces the
		// per-frame stream of a drag into a single command.
		struct FieldEditSink
		{
			UndoStack* undo = nullptr;
			std::uint32_t entityId = 0;
			const std::string* componentName = nullptr;

			void Record(const reflect::FieldDesc& f, const reflect::FieldValue& before, const reflect::FieldValue& after) const
			{
				if (undo == nullptr || componentName == nullptr)
				{
					return;
				}
				undo->RecordFieldEdit(entityId, *componentName, f.name, FieldValueToJson(before, &f), FieldValueToJson(after, &f), /*isReflected=*/true);
			}
		};

		// Do two entities hold the same value for one field? Compared field-by-field rather
		// than through the JSON codec: this runs for every drawn field against every other
		// selected entity, every frame, so it must not allocate.
		bool SameFieldValue(const reflect::FieldValue& a, const reflect::FieldValue& b)
		{
			if (a.type != b.type)
			{
				return false;
			}
			switch (a.type)
			{
				case FieldType::Bool:
					return a.boolean == b.boolean;
				case FieldType::Enum:
					return a.enumValue == b.enumValue;
				case FieldType::EntityRef:
					return a.entity == b.entity;
				case FieldType::String:
					return a.str == b.str;
				case FieldType::Color3:
				case FieldType::Color4:
				case FieldType::Vec2:
				case FieldType::Vec3:
				case FieldType::Vec4:
					return a.vec == b.vec;
				case FieldType::List:
				{
					if (a.list.size() != b.list.size())
					{
						return false;
					}
					for (std::size_t row = 0; row < a.list.size(); ++row)
					{
						if (a.list[row].size() != b.list[row].size())
						{
							return false;
						}
						for (std::size_t col = 0; col < a.list[row].size(); ++col)
						{
							if (!SameFieldValue(a.list[row][col], b.list[row][col]))
							{
								return false;
							}
						}
					}
					return true;
				}
				default:
					return a.num == b.num; // Float / Int / UInt
			}
		}

		bool DrawField(const reflect::FieldDesc& f, void* comp, const FieldEditSink& sink)
		{
			reflect::FieldValue v = f.get(comp);
			const reflect::FieldValue before = v; // the widget mutates v in place
			const bool changed = f.type == FieldType::List ? DrawListField(f, v) : DrawScalarField(f.name.c_str(), f.type, f.meta, v);
			if (changed)
			{
				f.set(comp, v);
				sink.Record(f, before, v);
			}
			return changed;
		}

		void DrawReflectedComponent(World& world, Entity entity, const reflect::ComponentType& rt, ServiceContainer& services, UndoStack* undo)
		{
			void* comp = rt.tryGetRaw(world, entity);
			if (comp == nullptr)
			{
				return;
			}

			ImGui::PushID(rt.name.c_str());
			bool removed = false;
			const std::string label = rt.icon + "  " + rt.name;
			const std::string removeId = std::string(ICON_FA_XMARK) + "##reflremove_" + rt.name;
			const bool open = iw::RemovableSection(label.c_str(), removeId.c_str(), removed, 0, MenuFor(services, world, entity, rt.name.c_str()));
			if (removed)
			{
				// Snapshot the fields first so undo restores the component's values,
				// not just its presence.
				if (undo != nullptr)
				{
					nlohmann::json snapshot;
					bool isReflected = false;
					CaptureComponentFields(world, entity, rt.name, services, snapshot, isReflected);
					rt.remove(world, entity);
					undo->Record(std::make_unique<RemoveComponentCommand>(entity.id, rt.name, std::move(snapshot), isReflected));
				}
				else
				{
					rt.remove(world, entity);
				}
				ImGui::PopID();
				return;
			}
			if (open)
			{
				const FieldEditSink sink{undo, entity.id, &rt.name};
				// Editing a field with several entities selected sets it on all of them that
				// carry this component - the same field, the same value. Unlike a transform
				// (which propagates a delta, so entities keep their relative offsets) an
				// intensity or a colour has no meaningful "relative" version, so the edited
				// value is applied outright. Entities without the component are left alone.
				const auto* selection = services.TryGet<SceneSelection>();
				const bool propagate = selection != nullptr && selection->All().size() > 1 && selection->Contains(entity);
				bool anyChanged = false;
				for (const auto& f: rt.fields)
				{
					// Warn before the value is flattened, not after: the widget shows the
					// primary's number, so without this a field where the selection disagrees
					// looks unanimous and one drag silently overwrites the rest.
					bool mixed = false;
					if (propagate)
					{
						const reflect::FieldValue mine = f.get(comp);
						for (const Entity other: selection->All())
						{
							if (other == entity || !world.GetRegistry().valid(World::ToEntt(other)))
							{
								continue;
							}
							const void* otherComp = rt.tryGetRawConst(world, other);
							if (otherComp != nullptr && !SameFieldValue(mine, f.get(otherComp)))
							{
								mixed = true;
								break;
							}
						}
					}
					if (mixed)
					{
						ImGui::PushStyleColor(ImGuiCol_Text, iw::ToImVec4(colors::Orange));
					}
					const bool fieldChanged = DrawField(f, comp, sink);
					if (mixed)
					{
						ImGui::PopStyleColor();
						ImGui::SetItemTooltip("Differs across the selection - showing %s. Editing this sets it on all of them.", EntityDisplayName(world, entity));
					}
					if (!fieldChanged)
					{
						continue;
					}
					anyChanged = true;
					if (!propagate)
					{
						continue;
					}
					const reflect::FieldValue edited = f.get(comp);
					for (const Entity other: selection->All())
					{
						if (other == entity || !world.GetRegistry().valid(World::ToEntt(other)))
						{
							continue;
						}
						void* otherComp = rt.tryGetRaw(world, other);
						if (otherComp == nullptr)
						{
							continue;
						}
						f.set(otherComp, edited);
						if (rt.postSet)
						{
							rt.postSet(world, other);
						}
					}
				}
				if (anyChanged && rt.postSet)
				{
					rt.postSet(world, entity);
				}
			}
			ImGui::PopID();
		}
	} // namespace

	std::vector<ReflectedComponentFields> CaptureReflectedFields(World& world, Entity entity, ServiceContainer& services)
	{
		std::vector<ReflectedComponentFields> snapshot;
		for (const reflect::ComponentType& rt: reflect::ComponentTypes())
		{
			const void* comp = rt.tryGetRawConst(world, entity);
			if (comp == nullptr)
			{
				continue;
			}
			nlohmann::json fields = nlohmann::json::object();
			for (const auto& f: rt.fields)
			{
				fields[f.name] = FieldValueToJson(f.get(comp), &f);
			}
			snapshot.push_back({rt.name, std::move(fields), /*isReflected=*/true});
		}
		// Hand-authored sets (currently just Material) are not in the reflection
		// registry but are edited by the same bespoke drawers.
		for (const ComponentFieldSet& set: ComponentFieldSets())
		{
			nlohmann::json fields = nlohmann::json::object();
			if (set.read(world, entity, services, fields))
			{
				snapshot.push_back({set.name, std::move(fields), /*isReflected=*/false});
			}
		}
		return snapshot;
	}

	void RecordReflectedFieldEdits(UndoStack& undo, World& world, Entity entity, ServiceContainer& services, const std::vector<ReflectedComponentFields>& before)
	{
		for (const ReflectedComponentFields& entry: before)
		{
			if (!entry.isReflected)
			{
				const ComponentFieldSet* set = FindComponentFields(entry.name);
				nlohmann::json now = nlohmann::json::object();
				if (set == nullptr || !set->read(world, entity, services, now))
				{
					continue;
				}
				for (const auto& [key, value]: now.items())
				{
					const auto it = entry.fields.find(key);
					if (it != entry.fields.end() && *it != value)
					{
						undo.RecordFieldEdit(entity.id, entry.name, key, *it, value, /*isReflected=*/false);
					}
				}
				continue;
			}
			const reflect::ComponentType* rt = reflect::FindComponentType(entry.name);
			if (rt == nullptr)
			{
				continue;
			}
			const void* comp = rt->tryGetRawConst(world, entity);
			if (comp == nullptr)
			{
				continue; // removed this frame; component lifetime is recorded elsewhere
			}
			for (const auto& f: rt->fields)
			{
				const auto it = entry.fields.find(f.name);
				if (it == entry.fields.end())
				{
					continue;
				}
				const nlohmann::json now = FieldValueToJson(f.get(comp), &f);
				if (*it != now)
				{
					undo.RecordFieldEdit(entity.id, entry.name, f.name, *it, now, /*isReflected=*/true);
				}
			}
		}
	}

	std::vector<ScriptEntry> CaptureScripts(World& world, Entity entity)
	{
		const auto* sc = world.TryGet<ScriptComponent>(entity);
		return sc != nullptr ? sc->scripts : std::vector<ScriptEntry>{};
	}

	void RecordScriptEdits(UndoStack& undo, World& world, Entity entity, const std::vector<ScriptEntry>& before)
	{
		const std::vector<ScriptEntry> after = CaptureScripts(world, entity);
		if (!ScriptListsEqual(before, after))
		{
			undo.Record(std::make_unique<SetScriptsCommand>(entity.id, before, after));
		}
	}

	std::vector<std::uint32_t> CaptureTags(World& world, Entity entity)
	{
		std::vector<std::uint32_t> tags;
		ForEachTag(
		        [&](const std::string&, std::uint32_t tagId)
		        {
			        if (TagHas(&world, entity.id, tagId))
			        {
				        tags.push_back(tagId);
			        }
		        });
		return tags;
	}

	void RecordTagEdits(UndoStack& undo, World& world, Entity entity, const std::vector<std::uint32_t>& before)
	{
		const std::vector<std::uint32_t> after = CaptureTags(world, entity);
		if (before != after)
		{
			undo.Record(std::make_unique<SetTagsCommand>(entity.id, before, after));
		}
	}

	void DrawReflectedComponents(World& world, Entity entity, ServiceContainer& services, std::initializer_list<std::string_view> exclude)
	{
		auto* undo = services.TryGet<UndoStack>();
		for (const reflect::ComponentType& rt: reflect::ComponentTypes())
		{
			bool skip = false;
			for (const std::string_view name: exclude)
			{
				if (rt.name == name)
				{
					skip = true;
					break;
				}
			}
			if (!skip)
			{
				DrawReflectedComponent(world, entity, rt, services, undo);
			}
		}
	}
} // namespace aether::editor
