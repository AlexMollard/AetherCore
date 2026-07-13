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

		// One editable row per field, chosen by FieldType. Reads the current value,
		// renders the widget seeded with it, and on change writes back through the
		// field's type-erased setter.
		bool DrawField(const reflect::FieldDesc& f, void* comp)
		{
			reflect::FieldValue v = f.get(comp);
			const char* lbl = f.name.c_str();
			const float speed = f.meta.speed > 0.0f ? f.meta.speed : 0.05f;
			bool changed = false;
			switch (f.type)
			{
				case FieldType::Float:
				{
					float x = static_cast<float>(v.num);
					if (iw::PropFloat(lbl, &x, speed, f.meta.min, f.meta.max))
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
					if (f.meta.enumTable != nullptr)
					{
						std::vector<const char*> items;
						int cur = 0;
						for (std::size_t i = 0; i < f.meta.enumTable->values.size(); ++i)
						{
							items.push_back(f.meta.enumTable->values[i].first.c_str());
							if (f.meta.enumTable->values[i].second == v.enumValue)
							{
								cur = static_cast<int>(i);
							}
						}
						if (iw::PropCombo(lbl, &cur, items.data(), static_cast<int>(items.size())) && cur >= 0 && cur < static_cast<int>(f.meta.enumTable->values.size()))
						{
							v.enumValue = f.meta.enumTable->values[static_cast<std::size_t>(cur)].second;
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
			}
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
