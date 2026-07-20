#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <entt/entt.hpp>
#include <toml++/toml.hpp>

#include "utils/BinaryReader.hpp"
#include "utils/BinaryWriter.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

#include "scene/SceneSerializerDetail.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		const char* SceneKindName(SceneKind kind)
		{
			switch (kind)
			{
				case SceneKind::Scene2D:
					return "2d";
				case SceneKind::Mixed:
					return "mixed";
				case SceneKind::Scene3D:
				default:
					return "3d";
			}
		}

		SceneKind SceneKindFromName(std::string_view name)
		{
			if (name == "2d")
			{
				return SceneKind::Scene2D;
			}
			if (name == "mixed")
			{
				return SceneKind::Mixed;
			}
			return SceneKind::Scene3D;
		}

		void AppendSceneFeature(toml::array& values, SceneFeatureFlags features, SceneFeatureFlags feature, std::string_view name)
		{
			if (HasSceneFeature(features, feature))
			{
				values.push_back(std::string{name});
			}
		}

		toml::array SceneFeaturesToToml(SceneFeatureFlags features)
		{
			toml::array values;
			AppendSceneFeature(values, features, SceneFeatureFlags::Sprites, "sprites");
			AppendSceneFeature(values, features, SceneFeatureFlags::Tilemaps, "tilemaps");
			AppendSceneFeature(values, features, SceneFeatureFlags::Physics2D, "physics_2d");
			AppendSceneFeature(values, features, SceneFeatureFlags::Meshes3D, "meshes_3d");
			AppendSceneFeature(values, features, SceneFeatureFlags::Lighting3D, "lighting_3d");
			AppendSceneFeature(values, features, SceneFeatureFlags::Navigation, "navigation");
			AppendSceneFeature(values, features, SceneFeatureFlags::Physics3D, "physics_3d");
			return values;
		}

		SceneFeatureFlags SceneFeaturesFromToml(const toml::array* values, SceneKind kind)
		{
			if (values == nullptr)
			{
				return DefaultSceneFeatures(kind);
			}

			SceneFeatureFlags features = SceneFeatureFlags::None;
			for (const toml::node& value: *values)
			{
				const std::optional<std::string_view> name = value.value<std::string_view>();
				if (!name.has_value())
				{
					continue;
				}
				if (*name == "sprites")
				{
					features |= SceneFeatureFlags::Sprites;
				}
				else if (*name == "tilemaps")
				{
					features |= SceneFeatureFlags::Tilemaps;
				}
				else if (*name == "physics_2d")
				{
					features |= SceneFeatureFlags::Physics2D;
				}
				else if (*name == "meshes_3d")
				{
					features |= SceneFeatureFlags::Meshes3D;
				}
				else if (*name == "lighting_3d")
				{
					features |= SceneFeatureFlags::Lighting3D;
				}
				else if (*name == "navigation")
				{
					features |= SceneFeatureFlags::Navigation;
				}
				else if (*name == "physics_3d")
				{
					features |= SceneFeatureFlags::Physics3D;
				}
				else
				{
					AE_WARN(LogCategory::App, "Ignoring unknown scene feature '{}'.", *name);
				}
			}
			return features;
		}

		void MigrateSceneFeaturesToV10(toml::table& root)
		{
			toml::table* scene = root["scene"].as_table();
			if (scene == nullptr || scene->contains("features"))
			{
				return;
			}
			const SceneKind kind = SceneKindFromName((*scene)["kind"].value_or(std::string{"3d"}));
			scene->insert("features", SceneFeaturesToToml(DefaultSceneFeatures(kind)));
		}

		// v14 made system activation and editor menus feature driven and added the
		// Physics3D flag. Scenes written earlier never recorded their kind's
		// physics flag, so grant it: physics_3d to 3D/Mixed scenes, physics_2d to
		// 2D scenes (both are now kind defaults).
		void MigratePhysicsFeaturesToV14(toml::table& root)
		{
			toml::table* scene = root["scene"].as_table();
			if (scene == nullptr)
			{
				return;
			}
			const SceneKind kind = SceneKindFromName((*scene)["kind"].value_or(std::string{"3d"}));
			toml::array* features = (*scene)["features"].as_array();
			if (features == nullptr)
			{
				return; // pre-v10 file: the v10 migration inserts kind defaults, which now include the physics flag
			}
			const std::string_view grant = kind == SceneKind::Scene2D ? "physics_2d" : "physics_3d";
			for (const toml::node& value: *features)
			{
				if (value.value<std::string_view>() == grant)
				{
					return;
				}
			}
			features->push_back(std::string{grant});
		}

		// v15 made Tilemaps a Scene2D default feature; grant it to older 2D
		// scenes so their editors show the tile tooling.
		void MigrateTilemapsFeatureToV15(toml::table& root)
		{
			toml::table* scene = root["scene"].as_table();
			if (scene == nullptr)
			{
				return;
			}
			if (SceneKindFromName((*scene)["kind"].value_or(std::string{"3d"})) != SceneKind::Scene2D)
			{
				return;
			}
			toml::array* features = (*scene)["features"].as_array();
			if (features == nullptr)
			{
				return; // pre-v10 file: the v10 migration inserts kind defaults, which now include tilemaps
			}
			for (const toml::node& value: *features)
			{
				if (value.value<std::string_view>() == "tilemaps")
				{
					return;
				}
			}
			features->push_back("tilemaps");
		}

		struct SceneMigration
		{
			int targetVersion;
			void (*apply)(toml::table&);
		};

		constexpr std::array kSceneMigrations{
		        SceneMigration{.targetVersion = 10, .apply = MigrateSceneFeaturesToV10},
		        SceneMigration{.targetVersion = 14, .apply = MigratePhysicsFeaturesToV14},
		        SceneMigration{.targetVersion = 15, .apply = MigrateTilemapsFeatureToV15},
		};

		void ApplySceneMigrations(toml::table& root, int sourceVersion)
		{
			for (const SceneMigration& migration: kSceneMigrations)
			{
				if (sourceVersion < migration.targetVersion)
				{
					migration.apply(root);
				}
			}
		}

		const char* ShapeName(PhysicsShapeType t)
		{
			switch (t)
			{
				case PhysicsShapeType::Sphere:
					return "sphere";
				case PhysicsShapeType::Capsule:
					return "capsule";
				case PhysicsShapeType::Cylinder:
					return "cylinder";
				case PhysicsShapeType::Box:
				default:
					return "box";
			}
		}

		PhysicsShapeType ShapeFromName(std::string_view s)
		{
			if (s == "sphere")
			{
				return PhysicsShapeType::Sphere;
			}
			if (s == "capsule")
			{
				return PhysicsShapeType::Capsule;
			}
			if (s == "cylinder")
			{
				return PhysicsShapeType::Cylinder;
			}
			return PhysicsShapeType::Box;
		}

		const char* JointTypeName(JointType t)
		{
			switch (t)
			{
				case JointType::Point:
					return "point";
				case JointType::Hinge:
					return "hinge";
				case JointType::Distance:
					return "distance";
				case JointType::Slider:
					return "slider";
				case JointType::Fixed:
				default:
					return "fixed";
			}
		}

		JointType JointTypeFromName(std::string_view s)
		{
			if (s == "point")
			{
				return JointType::Point;
			}
			if (s == "hinge")
			{
				return JointType::Hinge;
			}
			if (s == "distance")
			{
				return JointType::Distance;
			}
			if (s == "slider")
			{
				return JointType::Slider;
			}
			return JointType::Fixed;
		}

		const char* MotionName(PhysicsMotionType t)
		{
			switch (t)
			{
				case PhysicsMotionType::Static:
					return "static";
				case PhysicsMotionType::Kinematic:
					return "kinematic";
				case PhysicsMotionType::Dynamic:
				default:
					return "dynamic";
			}
		}

		PhysicsMotionType MotionFromName(std::string_view s)
		{
			if (s == "static")
			{
				return PhysicsMotionType::Static;
			}
			if (s == "kinematic")
			{
				return PhysicsMotionType::Kinematic;
			}
			return PhysicsMotionType::Dynamic;
		}

		toml::array Vec2ToToml(const glm::vec2& v)
		{
			return toml::array{v.x, v.y};
		}

		toml::array Vec3ToToml(const glm::vec3& v)
		{
			return toml::array{v.x, v.y, v.z};
		}

		toml::array Vec4ToToml(const glm::vec4& v)
		{
			return toml::array{v.x, v.y, v.z, v.w};
		}

		// The scene data model is entirely float32, but glm floats promote to double
		// when inserted into toml, which then serializes them at full double precision
		// (0.30000001192092896 for 0.3f) - visual noise, and a source of churn. Round
		// each float node to the shortest decimal that round-trips to the SAME float32
		// (lossless for float32 storage) and normalize -0.0 to 0.0.
		double CleanFloatForToml(double value)
		{
			const float f = static_cast<float>(value);
			if (f == 0.0f)
			{
				return 0.0; // fold -0.0 into 0.0
			}
			if (!std::isfinite(f))
			{
				return value; // leave inf/nan untouched
			}
			char buffer[32];
			const std::to_chars_result out = std::to_chars(buffer, buffer + sizeof(buffer), f);
			double cleaned = value;
			std::from_chars(buffer, out.ptr, cleaned);
			return cleaned;
		}

		// Walk a built document and clean every floating-point node in place. Integers
		// (guids, sprite ids, sorting layers) are stored as int64 and left alone.
		void CleanFloatsInTree(toml::node& node)
		{
			if (auto* table = node.as_table())
			{
				for (auto&& [key, child]: *table)
				{
					CleanFloatsInTree(child);
				}
			}
			else if (auto* array = node.as_array())
			{
				for (auto&& child: *array)
				{
					CleanFloatsInTree(child);
				}
			}
			else if (auto* dbl = node.as_floating_point())
			{
				dbl->get() = CleanFloatForToml(dbl->get());
			}
		}

		glm::vec2 Vec2FromToml(const toml::node_view<const toml::node>& node, glm::vec2 fallback)
		{
			if (const auto* arr = node.as_array(); arr && arr->size() >= 2)
			{
				return {static_cast<float>((*arr)[0].value_or(0.0)), static_cast<float>((*arr)[1].value_or(0.0))};
			}
			return fallback;
		}

		glm::vec3 Vec3FromToml(const toml::node_view<const toml::node>& node, glm::vec3 fallback)
		{
			if (const auto* arr = node.as_array(); arr && arr->size() >= 3)
			{
				return {static_cast<float>((*arr)[0].value_or(0.0)), static_cast<float>((*arr)[1].value_or(0.0)), static_cast<float>((*arr)[2].value_or(0.0))};
			}
			return fallback;
		}

		glm::vec4 Vec4FromToml(const toml::node_view<const toml::node>& node, glm::vec4 fallback)
		{
			if (const auto* arr = node.as_array(); arr && arr->size() >= 4)
			{
				return {static_cast<float>((*arr)[0].value_or(0.0)), static_cast<float>((*arr)[1].value_or(0.0)), static_cast<float>((*arr)[2].value_or(0.0)), static_cast<float>((*arr)[3].value_or(0.0))};
			}
			return fallback;
		}

		toml::table WriteReflectedToToml(std::string_view typeName, const void* comp)
		{
			toml::table tbl;
			const reflect::ComponentType* rt = reflect::FindComponentType(typeName);
			if (rt == nullptr)
			{
				return tbl;
			}
			for (const reflect::FieldDesc& f: rt->fields)
			{
				if (!f.meta.serialize)
				{
					continue;
				}
				const std::string key = f.meta.serializeName.empty() ? f.name : f.meta.serializeName;
				const reflect::FieldValue v = f.get(comp);
				switch (f.type)
				{
					case reflect::FieldType::Float:
						tbl.insert(key, f.meta.isAngleDegrees ? glm::radians(v.num) : v.num);
						break;
					case reflect::FieldType::Int:
					case reflect::FieldType::UInt:
						tbl.insert(key, static_cast<std::int64_t>(v.num));
						break;
					case reflect::FieldType::Bool:
						tbl.insert(key, v.boolean);
						break;
					case reflect::FieldType::Vec2:
						tbl.insert(key, Vec2ToToml(glm::vec2(v.vec)));
						break;
					case reflect::FieldType::Vec3:
					case reflect::FieldType::Color3:
						tbl.insert(key, Vec3ToToml(glm::vec3(v.vec)));
						break;
					case reflect::FieldType::Vec4:
					case reflect::FieldType::Color4:
						tbl.insert(key, Vec4ToToml(v.vec));
						break;
					case reflect::FieldType::Enum:
						tbl.insert(key, f.meta.enumTable != nullptr ? f.meta.enumTable->NameOf(v.enumValue) : std::to_string(v.enumValue));
						break;
					case reflect::FieldType::String:
						tbl.insert(key, v.str);
						break;
					case reflect::FieldType::EntityRef:
						tbl.insert(key, static_cast<std::int64_t>(v.entity));
						break;
				}
			}
			return tbl;
		}

		// `comp` must point at a value pre-initialized to the component's defaults;
		void ReadReflectedFromToml(std::string_view typeName, const toml::table& src, void* comp)
		{
			const reflect::ComponentType* rt = reflect::FindComponentType(typeName);
			if (rt == nullptr)
			{
				return;
			}
			const toml::node_view<const toml::node> view{src};
			for (const reflect::FieldDesc& f: rt->fields)
			{
				const std::string key = f.meta.serializeName.empty() ? f.name : f.meta.serializeName;
				const auto node = view[key];
				if (!node)
				{
					continue;
				}
				reflect::FieldValue v = f.get(comp);
				switch (f.type)
				{
					case reflect::FieldType::Float:
						v.num = f.meta.isAngleDegrees ? glm::degrees(node.value_or(glm::radians(v.num))) : node.value_or(v.num);
						break;
					case reflect::FieldType::Int:
					case reflect::FieldType::UInt:
						v.num = static_cast<double>(node.value_or(static_cast<std::int64_t>(v.num)));
						break;
					case reflect::FieldType::Bool:
						v.boolean = node.value_or(v.boolean);
						break;
					case reflect::FieldType::Vec2:
						v.vec = glm::vec4(Vec2FromToml(node, glm::vec2(v.vec)), 0.0f, 0.0f);
						break;
					case reflect::FieldType::Vec3:
					case reflect::FieldType::Color3:
						v.vec = glm::vec4(Vec3FromToml(node, glm::vec3(v.vec)), 0.0f);
						break;
					case reflect::FieldType::Vec4:
					case reflect::FieldType::Color4:
						v.vec = Vec4FromToml(node, v.vec);
						break;
					case reflect::FieldType::Enum:
						if (f.meta.enumTable != nullptr)
						{
							if (node.is_string())
							{
								v.enumValue = f.meta.enumTable->ValueOf(node.value_or(std::string{}), v.enumValue);
							}
							else
							{
								v.enumValue = static_cast<int>(node.value_or(static_cast<std::int64_t>(v.enumValue)));
							}
						}
						break;
					case reflect::FieldType::String:
						v.str = node.value_or(v.str);
						break;
					case reflect::FieldType::EntityRef:
						v.entity = static_cast<std::uint64_t>(node.value_or(static_cast<std::int64_t>(v.entity)));
						break;
				}
				f.set(comp, v);
			}
		}

		// TOML key a generically-serialized component is stored under: lower-cased
		// display name with spaces as underscores ("Material Pulse" -> "material_pulse"),
		// matching the keys the hand-written serializer used.
		std::string ComponentTomlKey(std::string_view displayName)
		{
			std::string key(displayName);
			for (char& c: key)
			{
				c = (c == ' ') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return key;
		}

		// One reflected field value <-> TOML (mirrors the switches in
		// Write/ReadReflectedFromToml, but from a stored FieldValue).
		void InsertReflectedFieldValue(toml::table& tbl, const std::string& key, const reflect::FieldDesc& f, const reflect::FieldValue& v)
		{
			switch (f.type)
			{
				case reflect::FieldType::Float: tbl.insert(key, f.meta.isAngleDegrees ? glm::radians(v.num) : v.num); break;
				case reflect::FieldType::Int:
				case reflect::FieldType::UInt: tbl.insert(key, static_cast<std::int64_t>(v.num)); break;
				case reflect::FieldType::Bool: tbl.insert(key, v.boolean); break;
				case reflect::FieldType::Vec2: tbl.insert(key, Vec2ToToml(glm::vec2(v.vec))); break;
				case reflect::FieldType::Vec3:
				case reflect::FieldType::Color3: tbl.insert(key, Vec3ToToml(glm::vec3(v.vec))); break;
				case reflect::FieldType::Vec4:
				case reflect::FieldType::Color4: tbl.insert(key, Vec4ToToml(v.vec)); break;
				case reflect::FieldType::Enum: tbl.insert(key, f.meta.enumTable != nullptr ? f.meta.enumTable->NameOf(v.enumValue) : std::to_string(v.enumValue)); break;
				case reflect::FieldType::String: tbl.insert(key, v.str); break;
				case reflect::FieldType::EntityRef: tbl.insert(key, static_cast<std::int64_t>(v.entity)); break;
			}
		}

		reflect::FieldValue ReadReflectedFieldValue(const toml::node_view<const toml::node>& node, const reflect::FieldDesc& f)
		{
			reflect::FieldValue v;
			v.type = f.type;
			switch (f.type)
			{
				case reflect::FieldType::Float: v.num = f.meta.isAngleDegrees ? glm::degrees(node.value_or(0.0)) : node.value_or(0.0); break;
				case reflect::FieldType::Int:
				case reflect::FieldType::UInt: v.num = static_cast<double>(node.value_or(std::int64_t{0})); break;
				case reflect::FieldType::Bool: v.boolean = node.value_or(false); break;
				case reflect::FieldType::Vec2: v.vec = glm::vec4(Vec2FromToml(node, glm::vec2(0.0f)), 0.0f, 0.0f); break;
				case reflect::FieldType::Vec3:
				case reflect::FieldType::Color3: v.vec = glm::vec4(Vec3FromToml(node, glm::vec3(0.0f)), 0.0f); break;
				case reflect::FieldType::Vec4:
				case reflect::FieldType::Color4: v.vec = Vec4FromToml(node, glm::vec4(0.0f)); break;
				case reflect::FieldType::Enum:
					if (f.meta.enumTable != nullptr)
					{
						v.enumValue = node.is_string() ? f.meta.enumTable->ValueOf(node.value_or(std::string{}), 0) : static_cast<int>(node.value_or(std::int64_t{0}));
					}
					break;
				case reflect::FieldType::String: v.str = node.value_or(std::string{}); break;
				case reflect::FieldType::EntityRef: v.entity = static_cast<std::uint64_t>(node.value_or(std::int64_t{0})); break;
			}
			return v;
		}

		const char* ScriptPropTypeTag(ScriptPropertyValue::Type type)
		{
			switch (type)
			{
				case ScriptPropertyValue::Type::Float:
					return "float";
				case ScriptPropertyValue::Type::Int:
					return "int";
				case ScriptPropertyValue::Type::Bool:
					return "bool";
				case ScriptPropertyValue::Type::Vector3:
					return "vec3";
				case ScriptPropertyValue::Type::String:
					return "string";
				case ScriptPropertyValue::Type::Enum:
					return "enum";
				case ScriptPropertyValue::Type::Entity:
					return "entity";
				case ScriptPropertyValue::Type::Component:
					return "component";
				case ScriptPropertyValue::Type::None:
				default:
					return "none";
			}
		}

		toml::table ScriptPropsToToml(const std::map<std::string, ScriptPropertyValue>& props)
		{
			toml::table out;
			for (const auto& [name, value]: props)
			{
				toml::table entry;
				entry.insert("t", ScriptPropTypeTag(value.type));
				switch (value.type)
				{
					case ScriptPropertyValue::Type::Float:
						entry.insert("v", static_cast<double>(value.f4[0]));
						break;
					case ScriptPropertyValue::Type::Int:
					case ScriptPropertyValue::Type::Enum:
					case ScriptPropertyValue::Type::Entity:
						entry.insert("v", static_cast<std::int64_t>(value.i64));
						break;
					case ScriptPropertyValue::Type::Component:
						// c = required component's catalog name (self-describing so the
						entry.insert("v", static_cast<std::int64_t>(value.i64));
						entry.insert("c", value.str);
						break;
					case ScriptPropertyValue::Type::Bool:
						entry.insert("v", value.i64 != 0);
						break;
					case ScriptPropertyValue::Type::Vector3:
						entry.insert("v", toml::array{value.f4[0], value.f4[1], value.f4[2]});
						break;
					case ScriptPropertyValue::Type::String:
						entry.insert("v", value.str);
						break;
					case ScriptPropertyValue::Type::None:
					default:
						break;
				}
				out.insert(name, std::move(entry));
			}
			return out;
		}

		std::map<std::string, ScriptPropertyValue> ScriptPropsFromToml(const toml::table& tbl)
		{
			std::map<std::string, ScriptPropertyValue> out;
			for (const auto& [key, node]: tbl)
			{
				const auto* entry = node.as_table();
				if (entry == nullptr)
				{
					continue;
				}
				const std::string tag = (*entry)["t"].value_or(std::string{});
				const auto value = (*entry)["v"];
				ScriptPropertyValue pv;
				if (tag == "float")
				{
					pv.type = ScriptPropertyValue::Type::Float;
					pv.f4[0] = static_cast<float>(value.value_or(0.0));
				}
				else if (tag == "int" || tag == "enum")
				{
					pv.type = tag == "enum" ? ScriptPropertyValue::Type::Enum : ScriptPropertyValue::Type::Int;
					pv.i64 = value.value_or(std::int64_t{0});
				}
				else if (tag == "entity")
				{
					pv.type = ScriptPropertyValue::Type::Entity;
					pv.i64 = value.value_or(std::int64_t{0});
				}
				else if (tag == "component")
				{
					pv.type = ScriptPropertyValue::Type::Component;
					pv.i64 = value.value_or(std::int64_t{0});
					pv.str = (*entry)["c"].value_or(std::string{});
				}
				else if (tag == "bool")
				{
					pv.type = ScriptPropertyValue::Type::Bool;
					pv.i64 = value.value_or(false) ? 1 : 0;
				}
				else if (tag == "vec3")
				{
					pv.type = ScriptPropertyValue::Type::Vector3;
					const glm::vec3 v = Vec3FromToml(value, glm::vec3(0.0f));
					pv.f4[0] = v.x;
					pv.f4[1] = v.y;
					pv.f4[2] = v.z;
				}
				else if (tag == "string")
				{
					pv.type = ScriptPropertyValue::Type::String;
					pv.str = value.value_or(std::string{});
				}
				else
				{
					continue;
				}
				out.emplace(std::string(key.str()), std::move(pv));
			}
			return out;
		}

	} // namespace

	toml::table BuildSceneToml(const SceneDescription& scene, bool includeSceneHeader = true)
	{
		toml::table root;
		// Prefabs are fragments, not scenes: they carry no [scene] header (kind /
		// features / version), matching the hand-authored convention. Writing one would
		// stamp a header-less prefab as kind='3d' with 3D features on any re-serialize.
		if (includeSceneHeader)
		{
			toml::table header;
			header.insert("version", kSceneFormatVersion);
			header.insert("name", scene.name);
			header.insert("kind", SceneKindName(scene.kind));
			header.insert("features", SceneFeaturesToToml(scene.features));
			root.insert("scene", std::move(header));
		}

		if (scene.environment)
		{
			const EnvironmentRecord& env = *scene.environment;
			toml::table e;
			e.insert("ambient", Vec3ToToml(env.ambient));
			e.insert("sun_direction", Vec3ToToml(env.sunDirection));
			e.insert("sun_intensity", env.sunIntensity);
			e.insert("sun_color", Vec3ToToml(env.sunColor));
			e.insert("sky_horizon", Vec3ToToml(env.skyHorizon));
			e.insert("sky_zenith", Vec3ToToml(env.skyZenith));
			e.insert("sky_void", Vec3ToToml(env.skyVoid));
			root.insert("environment", std::move(e));
		}

		// file that was never applied; fresh captures serialize per-entity.
		if (!scene.lights.empty())
		{
			toml::array lights;
			for (const LightRecord& light: scene.lights)
			{
				toml::table l;
				l.insert("type", light.isSpot ? "spot" : "point");
				l.insert("position", Vec3ToToml(light.position));
				l.insert("radius", light.radius);
				l.insert("color", Vec3ToToml(light.color));
				l.insert("intensity", light.intensity);
				l.insert("shadow", light.castsShadow);
				if (light.isSpot)
				{
					l.insert("direction", Vec3ToToml(light.direction));
					l.insert("inner_rad", light.innerAngleRad);
					l.insert("outer_rad", light.outerAngleRad);
				}
				lights.push_back(std::move(l));
			}
			root.insert("lights", std::move(lights));
		}

		const auto buildEntityTable = [](const EntityRecord& rec) -> toml::table
		{
			toml::table t;
			t.insert("name", rec.name);
			if (rec.guid != 0)
			{
				t.insert("guid", static_cast<std::int64_t>(rec.guid));
			}
			if (!rec.tags.empty())
			{
				toml::array tags;
				for (const auto& tag: rec.tags)
				{
					tags.push_back(tag);
				}
				t.insert("tags", std::move(tags));
			}
			if (rec.disabled)
			{
				t.insert("disabled", true);
			}
			if (rec.sprite)
			{
				toml::table sprite = WriteReflectedToToml("Sprite Renderer", &*rec.sprite);
				if (rec.sprite->spriteId.IsValid())
				{
					sprite.insert("sprite_id", static_cast<std::int64_t>(rec.sprite->spriteId.value));
				}
				t.insert("sprite", std::move(sprite));
			}
			if (rec.spriteAnimator)
			{
				t.insert("sprite_animator", WriteReflectedToToml("Sprite Animator", &*rec.spriteAnimator));
			}
			if (rec.meshRenderer)
			{
				t.insert("mesh_renderer", true);
				if (!rec.meshRendererVisible)
				{
					t.insert("mesh_renderer_visible", false);
				}
				if (!rec.meshRendererCastShadows)
				{
					t.insert("mesh_renderer_cast_shadows", false);
				}
			}
			t.insert("parent", rec.parentIndex);
			if (rec.hasTransform)
			{
				t.insert("position", Vec3ToToml(rec.position));
				t.insert("euler", Vec3ToToml(rec.eulerDeg));
				t.insert("scale", Vec3ToToml(rec.scale));
			}
			if (rec.mesh)
			{
				toml::table m;
				m.insert("kind", rec.mesh->kind == MeshSourceComponent::Kind::Model ? "model" : "primitive");
				m.insert("path", rec.mesh->path);
				m.insert("index", static_cast<std::int64_t>(rec.mesh->primitiveIndex));
				t.insert("mesh", std::move(m));
			}
			if (rec.material)
			{
				const MaterialAsset& a = rec.material->asset;
				toml::table m;
				m.insert("base_color", Vec4ToToml(a.baseColorFactor));
				m.insert("metallic", a.metallicFactor);
				m.insert("roughness", a.roughnessFactor);
				m.insert("occlusion", a.occlusionStrength);
				m.insert("alpha_cutoff", a.alphaCutoff);
				m.insert("emissive", Vec3ToToml(a.emissiveFactor));
				m.insert("double_sided", a.doubleSided);
				m.insert("alpha_blend", a.alphaBlend);
				m.insert("alpha_mask", a.alphaMask);
				m.insert("vertex_color", a.modulateVertexColor);
				m.insert("receive_shadows", a.receiveShadows);
				const auto tex = [&m](const char* key, const std::string& path)
				{
					if (!path.empty())
					{
						m.insert(key, path);
					}
				};
				tex("albedo", rec.material->albedoPath);
				tex("normal", rec.material->normalPath);
				tex("metallic_roughness", rec.material->metallicRoughnessPath);
				tex("occlusion_tex", rec.material->occlusionPath);
				tex("emissive_tex", rec.material->emissivePath);
				t.insert("material", std::move(m));
			}
			if (rec.skinned)
			{
				toml::table s;
				s.insert("clip", static_cast<std::int64_t>(rec.skinned->clipIndex));
				s.insert("time", rec.skinned->animTime);
				s.insert("speed", rec.skinned->playbackSpeed);
				s.insert("looping", rec.skinned->looping);
				t.insert("skinned", std::move(s));
			}
			if (rec.physics)
			{
				toml::table p;
				p.insert("shape", ShapeName(rec.physics->shapeType));
				p.insert("motion", MotionName(rec.physics->motionType));
				p.insert("half_extents", Vec3ToToml(rec.physics->halfExtents));
				p.insert("radius", rec.physics->radius);
				p.insert("half_height", rec.physics->halfHeight);
				p.insert("center", Vec3ToToml(rec.physics->center));
				p.insert("friction", rec.physics->friction);
				p.insert("restitution", rec.physics->restitution);
				p.insert("mass", rec.physics->mass);
				p.insert("linear_damping", rec.physics->linearDamping);
				p.insert("angular_damping", rec.physics->angularDamping);
				p.insert("gravity_factor", rec.physics->gravityFactor);
				p.insert("max_linear_vel", rec.physics->maxLinearVelocity);
				p.insert("max_angular_vel", rec.physics->maxAngularVelocity);
				p.insert("sensor", rec.physics->isSensor);
				p.insert("ccd", rec.physics->continuousCollision);
				p.insert("allow_sleeping", rec.physics->allowSleeping);
				p.insert("lock_position", Vec3ToToml(glm::vec3(rec.physics->lockPosition.x ? 1.0f : 0.0f, rec.physics->lockPosition.y ? 1.0f : 0.0f, rec.physics->lockPosition.z ? 1.0f : 0.0f)));
				p.insert("lock_rotation", Vec3ToToml(glm::vec3(rec.physics->lockRotation.x ? 1.0f : 0.0f, rec.physics->lockRotation.y ? 1.0f : 0.0f, rec.physics->lockRotation.z ? 1.0f : 0.0f)));
				t.insert("physics", std::move(p));
			}
			if (rec.joint)
			{
				toml::table j;
				j.insert("type", JointTypeName(rec.joint->type));
				j.insert("target", static_cast<std::int64_t>(rec.joint->targetIndex));
				j.insert("anchor", Vec3ToToml(rec.joint->anchor));
				j.insert("axis", Vec3ToToml(rec.joint->axis));
				j.insert("min_limit", rec.joint->minLimit);
				j.insert("max_limit", rec.joint->maxLimit);
				j.insert("distance", rec.joint->distance);
				j.insert("collide_connected", rec.joint->collideConnected);
				t.insert("joint", std::move(j));
			}
			if (rec.rigidBody2D)
			{
				t.insert("rigid_body_2d", WriteReflectedToToml("Rigid Body 2D", &*rec.rigidBody2D));
			}
			if (rec.collider2D)
			{
				toml::table c = WriteReflectedToToml("Collider 2D", &*rec.collider2D);
				if (!rec.collider2D->points.empty())
				{
					toml::array pts;
					for (const glm::vec2& p: rec.collider2D->points)
					{
						pts.push_back(Vec2ToToml(p));
					}
					c.insert("points", std::move(pts));
				}
				t.insert("collider_2d", std::move(c));
			}
			if (rec.joint2D)
			{
				toml::table j = WriteReflectedToToml("Joint 2D", &*rec.joint2D);
				// Reflection writes 'target' as a raw entity id, which does not
				// survive a cold load; overwrite with the scene-local index.
				j.insert_or_assign("target", static_cast<std::int64_t>(rec.joint2DTargetIndex));
				t.insert("joint_2d", std::move(j));
			}
			if (rec.uiCanvas)
			{
				toml::table c;
				c.insert("scale_mode", static_cast<std::int64_t>(rec.uiCanvas->scaleMode));
				c.insert("reference", Vec2ToToml(rec.uiCanvas->referenceResolution));
				c.insert("sort_bias", static_cast<std::int64_t>(rec.uiCanvas->sortBias));
				t.insert("ui_canvas", std::move(c));
			}
			if (rec.uiRect)
			{
				toml::table r;
				r.insert("anchor_min", Vec2ToToml(rec.uiRect->anchorMin));
				r.insert("anchor_max", Vec2ToToml(rec.uiRect->anchorMax));
				r.insert("offset_min", Vec2ToToml(rec.uiRect->offsetMin));
				r.insert("offset_max", Vec2ToToml(rec.uiRect->offsetMax));
				r.insert("pivot", Vec2ToToml(rec.uiRect->pivot));
				t.insert("ui_rect", std::move(r));
			}
			if (rec.uiImage)
			{
				toml::table im;
				im.insert("color", Vec4ToToml(rec.uiImage->color));
				im.insert("corner_radius", rec.uiImage->cornerRadius);
				im.insert("pixel_art", rec.uiImage->pixelArt);
				if (!rec.uiImage->texturePath.empty())
				{
					im.insert("texture", rec.uiImage->texturePath);
				}
				t.insert("ui_image", std::move(im));
			}
			if (rec.uiText)
			{
				toml::table tx;
				tx.insert("text", rec.uiText->text);
				tx.insert("font", rec.uiText->fontName);
				tx.insert("pixel_size", rec.uiText->pixelSize);
				tx.insert("color", Vec4ToToml(rec.uiText->color));
				tx.insert("h_align", static_cast<std::int64_t>(rec.uiText->hAlign));
				tx.insert("v_align", static_cast<std::int64_t>(rec.uiText->vAlign));
				tx.insert("wrap", rec.uiText->wrap);
				t.insert("ui_text", std::move(tx));
			}
			if (rec.effect)
			{
				toml::table f;
				f.insert("name", rec.effect->name);
				f.insert("tint", Vec4ToToml(rec.effect->params.tint));
				f.insert("speed", rec.effect->params.speed);
				f.insert("scale", rec.effect->params.scale);
				f.insert("intensity", rec.effect->params.intensity);
				t.insert("effect", std::move(f));
			}
			// Generically-serialized pure-data components (see GenericComponentTypeNames).
			for (const GenericComponent& generic: rec.reflected)
			{
				const reflect::ComponentType* ct = reflect::FindComponentType(generic.type);
				if (ct == nullptr)
				{
					continue;
				}
				toml::table gtbl;
				for (const auto& [fieldName, value]: generic.fields)
				{
					const reflect::FieldDesc* f = ct->FindField(fieldName);
					if (f == nullptr || !f->meta.serialize)
					{
						continue;
					}
					InsertReflectedFieldValue(gtbl, f->meta.serializeName.empty() ? f->name : f->meta.serializeName, *f, value);
				}
				t.insert(ComponentTomlKey(generic.type), std::move(gtbl));
			}
			if (rec.particles)
			{
				t.insert("particles", WriteReflectedToToml("Particle Emitter", &*rec.particles));
			}
			if (rec.pointLight)
			{
				t.insert("point_light", WriteReflectedToToml("Point Light", &*rec.pointLight));
			}
			if (rec.spotLight)
			{
				t.insert("spot_light", WriteReflectedToToml("Spot Light", &*rec.spotLight));
			}
			if (rec.dayNight)
			{
				t.insert("day_night", WriteReflectedToToml("Day Night", &*rec.dayNight));
			}
			if (rec.tileMap)
			{
				t.insert("tile_map", WriteReflectedToToml("Tile Map", &*rec.tileMap));
			}
			if (rec.camera)
			{
				toml::table c = WriteReflectedToToml("Camera", &*rec.camera);
				c.insert("main", rec.mainCamera);
				// Gradient stops are a variable-length list, which the reflection
				// field system does not model - serialize them explicitly.
				toml::array stops;
				for (const auto& s: rec.camera->gradientStops)
				{
					toml::table st;
					st.insert("colour", toml::array{s.colour.r, s.colour.g, s.colour.b});
					st.insert("position", s.position);
					stops.push_back(std::move(st));
				}
				c.insert("gradient_stops", std::move(stops));
				t.insert("camera", std::move(c));
			}
			if (rec.orbitCamera)
			{
				t.insert("orbit_camera", WriteReflectedToToml("Orbit Camera", &*rec.orbitCamera));
			}
			if (!rec.scripts.empty())
			{
				toml::array scripts;
				for (const ScriptRecord& script: rec.scripts)
				{
					toml::table s;
					s.insert("type", script.type);
					if (!script.properties.empty())
					{
						s.insert("properties", ScriptPropsToToml(script.properties));
					}
					scripts.push_back(std::move(s));
				}
				t.insert("scripts", std::move(scripts));
			}
			return t;
		};

		toml::array entities;
		for (const EntityRecord& rec: scene.entities)
		{
			entities.push_back(buildEntityTable(rec));
		}
		root.insert("entities", std::move(entities));

		if (!scene.assetManifest.empty())
		{
			toml::array assets;
			for (const AssetManifestEntry& a: scene.assetManifest)
			{
				toml::table at;
				at.insert("id", a.id);
				at.insert("type", a.type);
				at.insert("path", a.path);
				if (a.subIndex >= 0)
				{
					at.insert("sub", a.subIndex);
				}
				if (a.builtin)
				{
					at.insert("builtin", true);
				}
				assets.push_back(std::move(at));
			}
			root.insert("assets", std::move(assets));
		}

		if (!scene.prefabInstances.empty())
		{
			toml::array instances;
			for (const PrefabInstanceRecord& pi: scene.prefabInstances)
			{
				toml::table it;
				it.insert("prefab", pi.prefabPath);
				if (!pi.name.empty())
				{
					it.insert("name", pi.name);
				}
				it.insert("position", Vec3ToToml(pi.position));
				it.insert("euler", Vec3ToToml(pi.eulerDeg));
				it.insert("scale", Vec3ToToml(pi.scale));
				if (!pi.overrides.empty())
				{
					toml::array ovs;
					for (const PrefabEntityOverride& ov: pi.overrides)
					{
						toml::table ot;
						if (!ov.partialToml.empty())
						{
							try
							{
								ot = toml::parse(ov.partialToml);
							}
							catch (const toml::parse_error& err)
							{
								AE_WARN(LogCategory::App, "Prefab override serialize error: {}", err.description());
							}
						}
						ot.insert_or_assign("guid", static_cast<std::int64_t>(ov.guid));
						ovs.push_back(std::move(ot));
					}
					it.insert("overrides", std::move(ovs));
				}
				if (!pi.removedGuids.empty())
				{
					toml::array rem;
					for (const std::uint64_t g: pi.removedGuids)
					{
						rem.push_back(static_cast<std::int64_t>(g));
					}
					it.insert("removed", std::move(rem));
				}
				if (!pi.addedEntities.empty())
				{
					toml::array added;
					for (const EntityRecord& r: pi.addedEntities)
					{
						added.push_back(buildEntityTable(r));
					}
					it.insert("added", std::move(added));
				}
				instances.push_back(std::move(it));
			}
			root.insert("prefab_instances", std::move(instances));
		}

		// Normalize every float to its shortest float32 representation so both the TOML
		// text and the cooked binary (both built from this tree) are clean and stable.
		CleanFloatsInTree(root);
		return root;
	}

	namespace
	{
		std::string StringifyTree(const toml::table& tree)
		{
			std::ostringstream out;
			out << "# AetherCore scene - generated by the debug editor\n" << tree << "\n";
			return out.str();
		}
	} // namespace

	std::string WriteToml(const SceneDescription& scene, bool includeSceneHeader)
	{
		return StringifyTree(BuildSceneToml(scene, includeSceneHeader));
	}

	// Build a SceneDescription from an already-parsed TOML document (shared by the
	// text parser and the binary reader, which decodes the same document tree).
	std::optional<SceneDescription> BuildSceneFromToml(toml::table root)
	{
		const int sourceVersion = static_cast<int>(root["scene"]["version"].value_or(std::int64_t{1}));
		ApplySceneMigrations(root, sourceVersion);

		SceneDescription scene;
		scene.name = root["scene"]["name"].value_or(std::string{});
		scene.version = sourceVersion;
		scene.kind = SceneKindFromName(root["scene"]["kind"].value_or(std::string{"3d"}));
		scene.features = SceneFeaturesFromToml(root["scene"]["features"].as_array(), scene.kind);
		// Only nudge for real, named scene files. Prefabs and in-memory snapshots are
		// nameless and get instantiated constantly at runtime, so warning on those
		// just floods the log (and their older format is migrated on load anyway).
		if (scene.version < kSceneFormatVersion && !scene.name.empty())
		{
			AE_WARN(LogCategory::App,
			        "Scene file '{}' is format v{} (current v{}): records added since it was written are absent (v2 added behaviors + lights/environment; v3 made lights entities - legacy [[lights]] migrate on load). Re-save from the editor to "
			        "upgrade (v10 added persistent scene feature flags with kind-aware defaults).",
			        scene.name,
			        scene.version,
			        kSceneFormatVersion);
		}

		if (const auto* e = root["environment"].as_table())
		{
			const toml::node_view<const toml::node> ev{*e};
			EnvironmentRecord env;
			env.ambient = Vec3FromToml(ev["ambient"], env.ambient);
			env.sunDirection = Vec3FromToml(ev["sun_direction"], env.sunDirection);
			env.sunIntensity = static_cast<float>(ev["sun_intensity"].value_or(1.0));
			env.sunColor = Vec3FromToml(ev["sun_color"], env.sunColor);
			env.skyHorizon = Vec3FromToml(ev["sky_horizon"], env.skyHorizon);
			env.skyZenith = Vec3FromToml(ev["sky_zenith"], env.skyZenith);
			env.skyVoid = Vec3FromToml(ev["sky_void"], env.skyVoid);
			scene.environment = env;
		}

		if (const auto* lights = root["lights"].as_array())
		{
			for (const auto& node: *lights)
			{
				const auto* l = node.as_table();
				if (l == nullptr)
				{
					continue;
				}
				const toml::node_view<const toml::node> lv{*l};
				LightRecord light;
				light.isSpot = lv["type"].value_or(std::string{"point"}) == "spot";
				light.position = Vec3FromToml(lv["position"], light.position);
				light.radius = static_cast<float>(lv["radius"].value_or(1.0));
				light.color = Vec3FromToml(lv["color"], light.color);
				light.intensity = static_cast<float>(lv["intensity"].value_or(1.0));
				light.castsShadow = lv["shadow"].value_or(false);
				light.direction = Vec3FromToml(lv["direction"], light.direction);
				light.innerAngleRad = static_cast<float>(lv["inner_rad"].value_or(0.35));
				light.outerAngleRad = static_cast<float>(lv["outer_rad"].value_or(0.60));
				scene.lights.push_back(light);
			}
		}

		if (const auto* assets = root["assets"].as_array())
		{
			for (const auto& node: *assets)
			{
				const auto* at = node.as_table();
				if (at == nullptr)
				{
					continue;
				}
				AssetManifestEntry entry;
				entry.id = (*at)["id"].value_or(std::string{});
				entry.type = (*at)["type"].value_or(std::string{});
				entry.path = (*at)["path"].value_or(std::string{});
				entry.subIndex = static_cast<int>((*at)["sub"].value_or(std::int64_t{-1}));
				entry.builtin = (*at)["builtin"].value_or(false);
				if (!entry.path.empty())
				{
					scene.assetManifest.push_back(std::move(entry));
				}
			}
		}

		const auto* entities = root["entities"].as_array();

		// The entity-table parser, reused for scene entities and for the entity
		// records embedded in prefab-instance overrides.
		const auto parseEntityTable = [](const toml::table& tbl) -> EntityRecord
		{
			const toml::node_view<const toml::node> tv{tbl};

			EntityRecord rec;
			rec.name = tv["name"].value_or(std::string{});
			rec.guid = static_cast<std::uint64_t>(tv["guid"].value_or(std::int64_t{0}));
			if (const auto* tags = tv["tags"].as_array())
			{
				for (const auto& tag: *tags)
				{
					if (auto s = tag.value<std::string>())
					{
						rec.tags.push_back(*s);
					}
				}
			}
			rec.disabled = tv["disabled"].value_or(false);
			const bool legacySprite = tv["sprite"].is_boolean() && tv["sprite"].value_or(false);
			if (legacySprite)
			{
				rec.sprite = SpriteRendererComponent{};
			}
			else if (const auto* sprite = tv["sprite"].as_table())
			{
				SpriteRendererComponent component{};
				ReadReflectedFromToml("Sprite Renderer", *sprite, &component);
				component.spriteId.value = static_cast<std::uint64_t>(toml::node_view<const toml::node>{*sprite}["sprite_id"].value_or(std::int64_t{0}));
				rec.sprite = std::move(component);
			}
			if (const auto* animator = tv["sprite_animator"].as_table())
			{
				SpriteAnimatorComponent component{};
				ReadReflectedFromToml("Sprite Animator", *animator, &component);
				rec.spriteAnimator = std::move(component);
			}
			rec.meshRenderer = tv["mesh_renderer"].value_or(false);
			rec.meshRendererVisible = tv["mesh_renderer_visible"].value_or(true);
			rec.meshRendererCastShadows = tv["mesh_renderer_cast_shadows"].value_or(true);
			rec.parentIndex = static_cast<int>(tv["parent"].value_or(std::int64_t{-1}));
			if (tv["position"] || tv["euler"] || tv["scale"])
			{
				rec.hasTransform = true;
				rec.position = Vec3FromToml(tv["position"], glm::vec3(0.0f));
				rec.eulerDeg = Vec3FromToml(tv["euler"], glm::vec3(0.0f));
				rec.scale = Vec3FromToml(tv["scale"], glm::vec3(1.0f));
			}
			if (const auto* m = tv["mesh"].as_table())
			{
				const toml::node_view<const toml::node> mv{*m};
				MeshSourceComponent ms;
				ms.kind = mv["kind"].value_or(std::string{"primitive"}) == "model" ? MeshSourceComponent::Kind::Model : MeshSourceComponent::Kind::Primitive;
				ms.path = mv["path"].value_or(std::string{});
				ms.primitiveIndex = static_cast<std::uint32_t>(mv["index"].value_or(std::int64_t{0}));
				rec.mesh = std::move(ms);
			}
			if (const auto* m = tv["material"].as_table())
			{
				const toml::node_view<const toml::node> mv{*m};
				MaterialRecord mat;
				mat.asset.baseColorFactor = Vec4FromToml(mv["base_color"], glm::vec4(1.0f));
				mat.asset.metallicFactor = static_cast<float>(mv["metallic"].value_or(0.0));
				mat.asset.roughnessFactor = static_cast<float>(mv["roughness"].value_or(0.5));
				mat.asset.occlusionStrength = static_cast<float>(mv["occlusion"].value_or(1.0));
				mat.asset.alphaCutoff = static_cast<float>(mv["alpha_cutoff"].value_or(0.5));
				mat.asset.emissiveFactor = Vec3FromToml(mv["emissive"], glm::vec3(0.0f));
				mat.asset.doubleSided = mv["double_sided"].value_or(false);
				mat.asset.alphaBlend = mv["alpha_blend"].value_or(false);
				mat.asset.alphaMask = mv["alpha_mask"].value_or(false);
				mat.asset.modulateVertexColor = mv["vertex_color"].value_or(false);
				mat.asset.receiveShadows = mv["receive_shadows"].value_or(true);
				mat.albedoPath = mv["albedo"].value_or(std::string{});
				mat.normalPath = mv["normal"].value_or(std::string{});
				mat.metallicRoughnessPath = mv["metallic_roughness"].value_or(std::string{});
				mat.occlusionPath = mv["occlusion_tex"].value_or(std::string{});
				mat.emissivePath = mv["emissive_tex"].value_or(std::string{});
				rec.material = std::move(mat);
				if (legacySprite && rec.sprite)
				{
					rec.sprite->texturePath = rec.material->albedoPath;
					rec.sprite->tint = rec.material->asset.baseColorFactor;
				}
			}
			if (const auto* s = tv["skinned"].as_table())
			{
				const toml::node_view<const toml::node> sv{*s};
				rec.skinned = SkinnedRecord{.clipIndex = static_cast<std::uint32_t>(sv["clip"].value_or(std::int64_t{0})),
				        .animTime = static_cast<float>(sv["time"].value_or(0.0)),
				        .playbackSpeed = static_cast<float>(sv["speed"].value_or(1.0)),
				        .looping = sv["looping"].value_or(true)};
			}
			if (const auto* p = tv["physics"].as_table())
			{
				const toml::node_view<const toml::node> pv{*p};
				const glm::vec3 lockPos = Vec3FromToml(pv["lock_position"], glm::vec3(0.0f));
				const glm::vec3 lockRot = Vec3FromToml(pv["lock_rotation"], glm::vec3(0.0f));
				rec.physics = PhysicsRecord{.shapeType = ShapeFromName(pv["shape"].value_or(std::string{"box"})),
				        .halfExtents = Vec3FromToml(pv["half_extents"], glm::vec3(0.5f)),
				        .radius = static_cast<float>(pv["radius"].value_or(0.5)),
				        .halfHeight = static_cast<float>(pv["half_height"].value_or(0.5)),
				        .motionType = MotionFromName(pv["motion"].value_or(std::string{"dynamic"})),
				        .center = Vec3FromToml(pv["center"], glm::vec3(0.0f)),
				        .friction = static_cast<float>(pv["friction"].value_or(0.5)),
				        .restitution = static_cast<float>(pv["restitution"].value_or(0.0)),
				        .mass = static_cast<float>(pv["mass"].value_or(0.0)),
				        .linearDamping = static_cast<float>(pv["linear_damping"].value_or(0.05)),
				        .angularDamping = static_cast<float>(pv["angular_damping"].value_or(0.05)),
				        .gravityFactor = static_cast<float>(pv["gravity_factor"].value_or(1.0)),
				        .maxLinearVelocity = static_cast<float>(pv["max_linear_vel"].value_or(500.0)),
				        .maxAngularVelocity = static_cast<float>(pv["max_angular_vel"].value_or(47.124)),
				        .isSensor = pv["sensor"].value_or(false),
				        .continuousCollision = pv["ccd"].value_or(false),
				        .allowSleeping = pv["allow_sleeping"].value_or(true),
				        .lockPosition = glm::bvec3(lockPos.x > 0.5f, lockPos.y > 0.5f, lockPos.z > 0.5f),
				        .lockRotation = glm::bvec3(lockRot.x > 0.5f, lockRot.y > 0.5f, lockRot.z > 0.5f)};
			}
			if (const auto* j = tv["joint"].as_table())
			{
				const toml::node_view<const toml::node> jv{*j};
				rec.joint = JointRecord{
				        .type = JointTypeFromName(jv["type"].value_or(std::string{"fixed"})),
				        .targetIndex = static_cast<int>(jv["target"].value_or(std::int64_t{-1})),
				        .anchor = Vec3FromToml(jv["anchor"], glm::vec3(0.0f)),
				        .axis = Vec3FromToml(jv["axis"], glm::vec3(0.0f, 1.0f, 0.0f)),
				        .minLimit = static_cast<float>(jv["min_limit"].value_or(0.0)),
				        .maxLimit = static_cast<float>(jv["max_limit"].value_or(0.0)),
				        .distance = static_cast<float>(jv["distance"].value_or(-1.0)),
				        .collideConnected = jv["collide_connected"].value_or(false),
				};
			}
			if (const auto* rb = tv["rigid_body_2d"].as_table())
			{
				RigidBody2DComponent component{};
				ReadReflectedFromToml("Rigid Body 2D", *rb, &component);
				rec.rigidBody2D = std::move(component);
			}
			if (const auto* col = tv["collider_2d"].as_table())
			{
				Collider2DComponent component{};
				ReadReflectedFromToml("Collider 2D", *col, &component);
				if (const auto* pts = toml::node_view<const toml::node>{*col}["points"].as_array())
				{
					component.points.reserve(pts->size());
					for (const auto& p: *pts)
					{
						if (const auto* v = p.as_array(); v != nullptr && v->size() >= 2)
						{
							component.points.emplace_back(static_cast<float>((*v)[0].value_or(0.0)), static_cast<float>((*v)[1].value_or(0.0)));
						}
					}
				}
				rec.collider2D = std::move(component);
			}
			if (const auto* j2d = tv["joint_2d"].as_table())
			{
				Joint2DComponent component{};
				ReadReflectedFromToml("Joint 2D", *j2d, &component);
				// 'target' in the file is a scene-local index (see the writer);
				// clear the raw value reflection deposited and stash the index.
				component.target = {};
				component.jointId = 0;
				rec.joint2DTargetIndex = static_cast<int>(toml::node_view<const toml::node>{*j2d}["target"].value_or(std::int64_t{-1}));
				rec.joint2D = std::move(component);
			}
			if (const auto* c = tv["ui_canvas"].as_table())
			{
				const toml::node_view<const toml::node> cv{*c};
				UICanvasRecord r;
				r.scaleMode = static_cast<std::uint8_t>(cv["scale_mode"].value_or(std::int64_t{0}));
				r.referenceResolution = Vec2FromToml(cv["reference"], r.referenceResolution);
				r.sortBias = static_cast<int>(cv["sort_bias"].value_or(std::int64_t{0}));
				rec.uiCanvas = r;
			}
			if (const auto* r = tv["ui_rect"].as_table())
			{
				const toml::node_view<const toml::node> rv{*r};
				UIRectRecord rect;
				rect.anchorMin = Vec2FromToml(rv["anchor_min"], rect.anchorMin);
				rect.anchorMax = Vec2FromToml(rv["anchor_max"], rect.anchorMax);
				rect.offsetMin = Vec2FromToml(rv["offset_min"], rect.offsetMin);
				rect.offsetMax = Vec2FromToml(rv["offset_max"], rect.offsetMax);
				rect.pivot = Vec2FromToml(rv["pivot"], rect.pivot);
				rec.uiRect = rect;
			}
			if (const auto* im = tv["ui_image"].as_table())
			{
				const toml::node_view<const toml::node> iv{*im};
				UIImageRecord r;
				r.color = Vec4FromToml(iv["color"], r.color);
				r.cornerRadius = static_cast<float>(iv["corner_radius"].value_or(0.0));
				r.pixelArt = iv["pixel_art"].value_or(false);
				r.texturePath = iv["texture"].value_or(std::string{});
				rec.uiImage = r;
			}
			if (const auto* tx = tv["ui_text"].as_table())
			{
				const toml::node_view<const toml::node> v{*tx};
				UITextRecord r;
				r.text = v["text"].value_or(std::string{});
				r.fontName = v["font"].value_or(std::string{"Roboto"});
				r.pixelSize = static_cast<float>(v["pixel_size"].value_or(24.0));
				r.color = Vec4FromToml(v["color"], r.color);
				r.hAlign = static_cast<std::uint8_t>(v["h_align"].value_or(std::int64_t{0}));
				r.vAlign = static_cast<std::uint8_t>(v["v_align"].value_or(std::int64_t{0}));
				r.wrap = v["wrap"].value_or(true);
				rec.uiText = r;
			}
			if (const auto* f = tv["effect"].as_table())
			{
				const toml::node_view<const toml::node> fv{*f};
				EffectRecord fx;
				fx.name = fv["name"].value_or(std::string{});
				fx.params.tint = Vec4FromToml(fv["tint"], glm::vec4(1.0f));
				fx.params.speed = static_cast<float>(fv["speed"].value_or(1.0));
				fx.params.scale = static_cast<float>(fv["scale"].value_or(1.0));
				fx.params.intensity = static_cast<float>(fv["intensity"].value_or(1.0));
				rec.effect = std::move(fx);
			}
			// Generically-serialized pure-data components (see GenericComponentTypeNames).
			for (const std::string& typeName: GenericComponentTypeNames())
			{
				const reflect::ComponentType* ct = reflect::FindComponentType(typeName);
				if (ct == nullptr)
				{
					continue;
				}
				const auto* gtbl = tv[ComponentTomlKey(typeName)].as_table();
				if (gtbl == nullptr)
				{
					continue;
				}
				GenericComponent generic;
				generic.type = typeName;
				const toml::node_view<const toml::node> gview{*gtbl};
				for (const reflect::FieldDesc& f: ct->fields)
				{
					const auto node = gview[f.meta.serializeName.empty() ? f.name : f.meta.serializeName];
					if (!node)
					{
						continue;
					}
					generic.fields.emplace_back(f.name, ReadReflectedFieldValue(node, f));
				}
				rec.reflected.push_back(std::move(generic));
			}
			if (const auto* p = tv["particles"].as_table())
			{
				ParticleEmitterComponent c{};
				ReadReflectedFromToml("Particle Emitter", *p, &c);
				rec.particles = c;
			}
			if (const auto* l = tv["point_light"].as_table())
			{
				PointLightComponent c{};
				ReadReflectedFromToml("Point Light", *l, &c);
				rec.pointLight = c;
			}
			if (const auto* l = tv["spot_light"].as_table())
			{
				SpotLightComponent c{};
				ReadReflectedFromToml("Spot Light", *l, &c);
				rec.spotLight = c;
			}
			if (const auto* dn = tv["day_night"].as_table())
			{
				DayNightComponent c{};
				ReadReflectedFromToml("Day Night", *dn, &c);
				rec.dayNight = c;
			}
			if (const auto* tm = tv["tile_map"].as_table())
			{
				TileMapComponent c{};
				ReadReflectedFromToml("Tile Map", *tm, &c);
				rec.tileMap = c;
			}
			if (const auto* c = tv["camera"].as_table())
			{
				CameraComponent cam{};
				ReadReflectedFromToml("Camera", *c, &cam);

				const toml::node_view<const toml::node> cv{*c};
				// Back-compat: pre-background scenes used a `use_sky_gradient` bool.
				if (!cv["background"] && cv["use_sky_gradient"])
				{
					cam.background = cv["use_sky_gradient"].value_or(true) ? CameraBackground::SkyGradient : CameraBackground::SolidColour;
				}
				// Gradient stops (custom: reflection has no list type).
				if (const auto* stops = cv["gradient_stops"].as_array())
				{
					cam.gradientStops.clear();
					for (const auto& node: *stops)
					{
						if (const auto* st = node.as_table())
						{
							GradientStop s{};
							if (const auto* col = (*st)["colour"].as_array(); col != nullptr && col->size() >= 3)
							{
								s.colour = glm::vec3((*col)[0].value_or(0.0f), (*col)[1].value_or(0.0f), (*col)[2].value_or(0.0f));
							}
							s.position = std::clamp((*st)["position"].value_or(0.0f), 0.0f, 1.0f);
							cam.gradientStops.push_back(s);
						}
					}
					std::sort(cam.gradientStops.begin(), cam.gradientStops.end(), [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
					if (cam.gradientStops.size() < 2)
					{
						cam.gradientStops = CameraComponent{}.gradientStops;
					}
				}

				rec.camera = cam;
				rec.mainCamera = cv["main"].value_or(false);
			}
			if (const auto* o = tv["orbit_camera"].as_table())
			{
				OrbitCameraComponent c{};
				ReadReflectedFromToml("Orbit Camera", *o, &c);
				rec.orbitCamera = c;
			}
			if (const auto* scripts = tv["scripts"].as_array())
			{
				for (const toml::node& scriptNode: *scripts)
				{
					const auto* scriptTable = scriptNode.as_table();
					if (scriptTable == nullptr)
					{
						continue;
					}
					const toml::node_view<const toml::node> sv{*scriptTable};
					const auto type = sv["type"].value<std::string>();
					if (!type.has_value() || type->empty())
					{
						continue;
					}
					ScriptRecord script;
					script.type = *type;
					if (const auto* props = sv["properties"].as_table())
					{
						script.properties = ScriptPropsFromToml(*props);
					}
					rec.scripts.push_back(std::move(script));
				}
			}
			else if (const auto script = tv["script"].value<std::string>(); script.has_value() && !script->empty())
			{
				ScriptRecord legacy;
				legacy.type = *script;
				if (const auto* props = tv["script_properties"].as_table())
				{
					legacy.properties = ScriptPropsFromToml(*props);
				}
				rec.scripts.push_back(std::move(legacy));
			}
			return rec;
		};

		if (entities != nullptr)
		{
			for (const auto& node: *entities)
			{
				if (const auto* t = node.as_table())
				{
					scene.entities.push_back(parseEntityTable(*t));
				}
			}
		}

		if (const auto* instances = root["prefab_instances"].as_array())
		{
			for (const auto& node: *instances)
			{
				const auto* it = node.as_table();
				if (it == nullptr)
				{
					continue;
				}
				PrefabInstanceRecord pi;
				pi.prefabPath = (*it)["prefab"].value_or(std::string{});
				if (pi.prefabPath.empty())
				{
					continue;
				}
				pi.name = (*it)["name"].value_or(std::string{});
				pi.position = Vec3FromToml((*it)["position"], glm::vec3(0.0f));
				pi.eulerDeg = Vec3FromToml((*it)["euler"], glm::vec3(0.0f));
				pi.scale = Vec3FromToml((*it)["scale"], glm::vec3(1.0f));
				if (const auto* ovs = (*it)["overrides"].as_array())
				{
					for (const auto& on: *ovs)
					{
						const auto* ot = on.as_table();
						if (ot == nullptr)
						{
							continue;
						}
						PrefabEntityOverride ov;
						ov.guid = static_cast<std::uint64_t>((*ot)["guid"].value_or(std::int64_t{0}));
						toml::table keys = *ot;
						keys.erase("guid");
						ov.partialToml = StringifyTree(keys);
						pi.overrides.push_back(std::move(ov));
					}
				}
				if (const auto* rem = (*it)["removed"].as_array())
				{
					for (const auto& g: *rem)
					{
						pi.removedGuids.push_back(static_cast<std::uint64_t>(g.value_or(std::int64_t{0})));
					}
				}
				if (const auto* added = (*it)["added"].as_array())
				{
					for (const auto& an: *added)
					{
						if (const auto* at = an.as_table())
						{
							pi.addedEntities.push_back(parseEntityTable(*at));
						}
					}
				}
				scene.prefabInstances.push_back(std::move(pi));
			}
		}

		return scene;
	}

	std::optional<SceneDescription> ParseToml(std::string_view text)
	{
		toml::table root;
		try
		{
			root = toml::parse(text);
		}
		catch (const toml::parse_error& err)
		{
			AE_WARN(LogCategory::App, "Scene parse error: {}", err.description());
			return std::nullopt;
		}
		return BuildSceneFromToml(std::move(root));
	}

	bool SceneTextHasNoCameraSource(std::string_view sceneToml)
	{
		const std::optional<SceneDescription> parsed = ParseToml(sceneToml);
		if (!parsed)
		{
			return false; // unparseable - a separate error path handles that
		}
		// A prefab instance may carry the main camera, so treat any instance as a
		// possible camera source and never warn then (avoids false positives).
		if (!parsed->prefabInstances.empty())
		{
			return false;
		}
		for (const EntityRecord& e: parsed->entities)
		{
			if (e.mainCamera)
			{
				return false;
			}
		}
		return true;
	}

	namespace
	{
		// Round-trip a single record through the scene document path to get / rebuild
		// its top-level TOML table, so override diff/merge is generic over every
		// component slot (no per-field code) and always matches on-disk serialization.
		toml::table EntityToTable(const EntityRecord& rec)
		{
			SceneDescription d;
			d.entities.push_back(rec);
			toml::table root = BuildSceneToml(d);
			if (auto* arr = root["entities"].as_array(); arr != nullptr && !arr->empty())
			{
				if (auto* t = (*arr)[0].as_table())
				{
					return *t;
				}
			}
			return {};
		}

		EntityRecord TableToEntity(toml::table entTable)
		{
			toml::table root;
			root.insert("version", static_cast<std::int64_t>(kSceneFormatVersion));
			toml::array arr;
			arr.push_back(std::move(entTable));
			root.insert("entities", std::move(arr));
			if (auto d = BuildSceneFromToml(std::move(root)); d.has_value() && !d->entities.empty())
			{
				return std::move(d->entities[0]);
			}
			return {};
		}

		// Canonical string form of a single node, for value comparison and copying.
		std::string NodeToString(const toml::node& n)
		{
			toml::table tmp;
			n.visit([&](auto&& concrete) { tmp.insert_or_assign("k", concrete); });
			return StringifyTree(tmp);
		}
	} // namespace

	std::string ComputePrefabOverrideToml(const EntityRecord& live, const EntityRecord& prefab)
	{
		const toml::table lt = EntityToTable(live);
		const toml::table pt = EntityToTable(prefab);
		toml::table pruned;
		for (auto&& [k, v]: lt)
		{
			const std::string key(k.str());
			if (key == "guid" || key == "parent")
			{
				continue; // identity / structural, never an override
			}
			const auto* pv = pt.get(key);
			if (pv == nullptr || NodeToString(v) != NodeToString(*pv))
			{
				v.visit([&](auto&& concrete) { pruned.insert_or_assign(key, concrete); });
			}
		}
		if (pruned.empty())
		{
			return {};
		}
		return StringifyTree(pruned);
	}

	EntityRecord MergePrefabOverride(const EntityRecord& prefab, const std::string& partialToml)
	{
		toml::table merged = EntityToTable(prefab);
		if (!partialToml.empty())
		{
			try
			{
				const toml::table over = toml::parse(partialToml);
				for (auto&& [k, v]: over)
				{
					const std::string key(k.str());
					if (key == "guid")
					{
						continue;
					}
					v.visit([&](auto&& concrete) { merged.insert_or_assign(key, concrete); });
				}
			}
			catch (const toml::parse_error& err)
			{
				AE_WARN(LogCategory::App, "Prefab override parse error: {}", err.description());
			}
		}
		return TableToEntity(std::move(merged));
	}

	// ── Binary format ────────────────────────────────────────────────────────
	// The binary scene/prefab format is the TOML document tree encoded as bytes
	// (like BSON to JSON): the SAME BuildSceneToml/BuildSceneFromToml path, just a
	// compact, tokenizer-free encoding for fast runtime loads. Any component that
	// serializes to TOML serializes to binary for free - no per-component code.
	namespace
	{
		constexpr std::uint32_t kSceneBinaryMagic = 0x42534541u; // "AESB"
		constexpr std::uint32_t kSceneBinaryVersion = 1u;

		enum class TomlTag : std::uint8_t
		{
			Null = 0,
			Table = 1,
			Array = 2,
			String = 3,
			Int = 4,
			Float = 5,
			Bool = 6,
		};

		void WriteTomlTreeNode(BinaryWriter& writer, const toml::node& node)
		{
			switch (node.type())
			{
				case toml::node_type::table:
				{
					const toml::table& table = *node.as_table();
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Table));
					writer.Write<std::uint32_t>(static_cast<std::uint32_t>(table.size()));
					for (auto&& [key, child]: table)
					{
						writer.WriteString(key.str());
						WriteTomlTreeNode(writer, child);
					}
					break;
				}
				case toml::node_type::array:
				{
					const toml::array& array = *node.as_array();
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Array));
					writer.Write<std::uint32_t>(static_cast<std::uint32_t>(array.size()));
					for (auto&& child: array)
					{
						WriteTomlTreeNode(writer, child);
					}
					break;
				}
				case toml::node_type::string:
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::String));
					writer.WriteString(node.as_string()->get());
					break;
				case toml::node_type::integer:
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Int));
					writer.Write<std::int64_t>(node.as_integer()->get());
					break;
				case toml::node_type::floating_point:
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Float));
					writer.Write<double>(node.as_floating_point()->get());
					break;
				case toml::node_type::boolean:
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Bool));
					writer.Write<std::uint8_t>(node.as_boolean()->get() ? 1u : 0u);
					break;
				default:
					writer.Write<std::uint8_t>(static_cast<std::uint8_t>(TomlTag::Null));
					break;
			}
		}

		std::string ReadBinaryString(BinaryReader& reader)
		{
			const std::uint32_t length = reader.Read<std::uint32_t>();
			std::string value;
			if (length > 0 && reader.CanRead(length))
			{
				value.assign(reinterpret_cast<const char*>(reader.Data()), length);
				reader.Advance(length);
			}
			return value;
		}

		// Plain (non-template) recursion: a generic-lambda template here would
		// instantiate a fresh type at every depth and blow up the compiler.
		toml::table ReadTomlTableBody(BinaryReader& reader, int depth);
		toml::array ReadTomlArrayBody(BinaryReader& reader, int depth);

		toml::table ReadTomlTableBody(BinaryReader& reader, int depth)
		{
			toml::table table;
			if (depth > 512)
			{
				return table;
			}
			const std::uint32_t count = reader.Read<std::uint32_t>();
			for (std::uint32_t i = 0; i < count && reader.Remaining() > 0; ++i)
			{
				const std::string key = ReadBinaryString(reader);
				switch (static_cast<TomlTag>(reader.Read<std::uint8_t>()))
				{
					case TomlTag::Table: table.insert(key, ReadTomlTableBody(reader, depth + 1)); break;
					case TomlTag::Array: table.insert(key, ReadTomlArrayBody(reader, depth + 1)); break;
					case TomlTag::String: table.insert(key, ReadBinaryString(reader)); break;
					case TomlTag::Int: table.insert(key, reader.Read<std::int64_t>()); break;
					case TomlTag::Float: table.insert(key, reader.Read<double>()); break;
					case TomlTag::Bool: table.insert(key, static_cast<bool>(reader.Read<std::uint8_t>())); break;
					case TomlTag::Null:
					default: break;
				}
			}
			return table;
		}

		toml::array ReadTomlArrayBody(BinaryReader& reader, int depth)
		{
			toml::array array;
			if (depth > 512)
			{
				return array;
			}
			const std::uint32_t count = reader.Read<std::uint32_t>();
			for (std::uint32_t i = 0; i < count && reader.Remaining() > 0; ++i)
			{
				switch (static_cast<TomlTag>(reader.Read<std::uint8_t>()))
				{
					case TomlTag::Table: array.push_back(ReadTomlTableBody(reader, depth + 1)); break;
					case TomlTag::Array: array.push_back(ReadTomlArrayBody(reader, depth + 1)); break;
					case TomlTag::String: array.push_back(ReadBinaryString(reader)); break;
					case TomlTag::Int: array.push_back(reader.Read<std::int64_t>()); break;
					case TomlTag::Float: array.push_back(reader.Read<double>()); break;
					case TomlTag::Bool: array.push_back(static_cast<bool>(reader.Read<std::uint8_t>())); break;
					case TomlTag::Null:
					default: break;
				}
			}
			return array;
		}
	} // namespace

	namespace
	{
		std::vector<std::byte> EncodeTreeBinary(const toml::table& tree)
		{
			BinaryWriter writer;
			writer.Write<std::uint32_t>(kSceneBinaryMagic);
			writer.Write<std::uint32_t>(kSceneBinaryVersion);
			WriteTomlTreeNode(writer, tree);
			return writer.Take();
		}
	} // namespace

	std::vector<std::byte> WriteSceneBinary(const SceneDescription& scene, bool includeSceneHeader)
	{
		return EncodeTreeBinary(BuildSceneToml(scene, includeSceneHeader));
	}

	void SerializeScene(const SceneDescription& scene, std::string& outToml, std::vector<std::byte>& outBinary, bool includeSceneHeader)
	{
		// Build the document tree ONCE and drive both encoders from it. Previously
		// SaveSceneFile called WriteToml and WriteSceneBinary separately, each of
		// which ran BuildSceneToml - doubling the serialization cost per save.
		const toml::table tree = BuildSceneToml(scene, includeSceneHeader);
		outToml = StringifyTree(tree);
		outBinary = EncodeTreeBinary(tree);
	}

	std::optional<SceneDescription> ReadSceneBinary(const std::byte* data, std::size_t size)
	{
		BinaryReader reader(data, size);
		if (!reader.CanRead(sizeof(std::uint32_t) * 2 + 1) || reader.Read<std::uint32_t>() != kSceneBinaryMagic || reader.Read<std::uint32_t>() != kSceneBinaryVersion)
		{
			return std::nullopt;
		}
		if (static_cast<TomlTag>(reader.Read<std::uint8_t>()) != TomlTag::Table)
		{
			return std::nullopt;
		}
		return BuildSceneFromToml(ReadTomlTableBody(reader, 0));
	}

	std::optional<SceneDescription> ReadSceneBinary(const std::vector<std::byte>& bytes)
	{
		return ReadSceneBinary(bytes.data(), bytes.size());
	}
} // namespace aether::app::scene
