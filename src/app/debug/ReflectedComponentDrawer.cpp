#include "debug/ReflectedComponentDrawer.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

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

		bool DrawField(const reflect::FieldDesc& f, void* comp)
		{
			reflect::FieldValue v = f.get(comp);
			const bool changed = f.type == FieldType::List ? DrawListField(f, v) : DrawScalarField(f.name.c_str(), f.type, f.meta, v);
			if (changed)
			{
				f.set(comp, v);
			}
			return changed;
		}

		void DrawReflectedComponent(World& world, Entity entity, const reflect::ComponentType& rt)
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
			const bool open = iw::RemovableSection(label.c_str(), removeId.c_str(), removed);
			if (removed)
			{
				rt.remove(world, entity);
				ImGui::PopID();
				return;
			}
			if (open)
			{
				bool anyChanged = false;
				for (const auto& f: rt.fields)
				{
					anyChanged |= DrawField(f, comp);
				}
				if (anyChanged && rt.postSet)
				{
					rt.postSet(world, entity);
				}
			}
			ImGui::PopID();
		}
	} // namespace

	void DrawReflectedComponents(World& world, Entity entity, std::initializer_list<std::string_view> exclude)
	{
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
				DrawReflectedComponent(world, entity, rt);
			}
		}
	}
} // namespace aether::editor
