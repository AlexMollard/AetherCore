#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>
#include <toml++/toml.hpp>

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

namespace aether::app::scene
{
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

		struct SceneMigration
		{
			int targetVersion;
			void (*apply)(toml::table&);
		};

		constexpr std::array kSceneMigrations{
		        SceneMigration{.targetVersion = 10, .apply = MigrateSceneFeaturesToV10},
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

		std::optional<PrimitiveMesh> PrimitiveFromName(std::string_view s)
		{
			if (s == "cube")
			{
				return PrimitiveMesh::Cube;
			}
			if (s == "sphere")
			{
				return PrimitiveMesh::Sphere;
			}
			if (s == "plane")
			{
				return PrimitiveMesh::Plane;
			}
			if (s == "quad")
			{
				return PrimitiveMesh::Quad;
			}
			if (s == "triangle")
			{
				return PrimitiveMesh::Triangle;
			}
			return std::nullopt;
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

		std::map<std::string, ScriptPropertyValue> ScriptPropsToSceneRefs(const std::map<std::string, ScriptPropertyValue>& props, const std::unordered_map<std::uint32_t, int>& indexOf)
		{
			std::map<std::string, ScriptPropertyValue> out = props;
			for (auto& [_, value]: out)
			{
				const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
				if (!isRef || value.i64 == 0)
				{
					continue;
				}
				const auto it = indexOf.find(static_cast<std::uint32_t>(value.i64));
				value.i64 = it != indexOf.end() ? it->second : 0;
			}
			return out;
		}

		std::map<std::string, ScriptPropertyValue> ScriptPropsFromSceneRefs(const std::map<std::string, ScriptPropertyValue>& props, const std::vector<Entity>& created)
		{
			std::map<std::string, ScriptPropertyValue> out = props;
			for (auto& [_, value]: out)
			{
				const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
				if (!isRef || value.i64 < 0 || static_cast<std::size_t>(value.i64) >= created.size())
				{
					if (isRef)
					{
						value.i64 = 0;
					}
					continue;
				}
				value.i64 = created[static_cast<std::size_t>(value.i64)].id;
			}
			return out;
		}

	} // namespace

	namespace
	{
		void AppendCaptureOrder(World& world, Entity entity, std::vector<Entity>& order, std::unordered_set<std::uint32_t>& seen)
		{
			if (!entity.IsValid() || seen.contains(entity.id) || !world.GetRegistry().valid(World::ToEntt(entity)) || ecs::HasSceneTransientAncestor(world, entity))
			{
				return;
			}

			seen.insert(entity.id);
			order.push_back(entity);
			if (const auto* h = world.TryGet<HierarchyComponent>(entity))
			{
				for (const Entity child: h->children)
				{
					AppendCaptureOrder(world, child, order, seen);
				}
			}
		}

		void AppendEntityRecords(SceneDescription& scene, World& world, const std::vector<Entity>& order, const std::unordered_map<std::uint32_t, int>& indexOf, const MaterialRegistry& materials, const TextureRegistry& textures)
		{
			scene.entities.reserve(scene.entities.size() + order.size());
			for (const Entity e: order)
			{
				EntityRecord rec;
				rec.entityId = e.id;
				if (const auto* nc = world.TryGet<NameComponent>(e))
				{
					rec.name = nc->name;
				}
				ForEachTag(
				        [&](const std::string& tagName, std::uint32_t tagId)
				        {
					        if (TagHas(&world, e.id, tagId))
					        {
						        rec.tags.push_back(tagName);
					        }
				        });
				rec.disabled = world.Has<DisabledComponent>(e);
				if (const auto* sprite = world.TryGet<SpriteRendererComponent>(e))
				{
					rec.sprite = *sprite;
				}
				if (const auto* mr = world.TryGet<MeshRendererComponent>(e))
				{
					rec.meshRenderer = true;
					rec.meshRendererVisible = mr->visible;
					rec.meshRendererCastShadows = mr->castShadows;
				}
				if (const auto* tc = world.TryGet<TransformComponent>(e))
				{
					rec.hasTransform = true;
					DecomposeTRS(tc->localToWorld, rec.position, rec.eulerDeg, rec.scale);
				}
				if (const auto* h = world.TryGet<HierarchyComponent>(e); h != nullptr && h->parent.IsValid())
				{
					const auto it = indexOf.find(h->parent.id);
					rec.parentIndex = it != indexOf.end() ? it->second : -1;
				}
				if (const auto* ms = world.TryGet<MeshSourceComponent>(e))
				{
					rec.mesh = *ms;
				}
				if (const auto* mc = world.TryGet<MaterialComponent>(e))
				{
					MaterialRecord mat;
					bool haveAsset = false;
					if (const auto* inst = world.TryGet<MaterialInstanceComponent>(e))
					{
						mat.asset = inst->asset;
						haveAsset = true;
					}
					else
					{
						haveAsset = materials.TryDescribe(mc->handle, mat.asset);
					}
					if (haveAsset)
					{
						const auto pathOf = [&textures](TextureHandle h, std::string& out)
						{
							if (h.IsValid() && h.index != TextureHandle::kBrokenIndex)
							{
								textures.TryGetPath(h, out);
							}
						};
						pathOf(mat.asset.albedoTex, mat.albedoPath);
						pathOf(mat.asset.normalTex, mat.normalPath);
						pathOf(mat.asset.metallicRoughnessTex, mat.metallicRoughnessPath);
						pathOf(mat.asset.occlusionTex, mat.occlusionPath);
						pathOf(mat.asset.emissiveTex, mat.emissivePath);
						rec.material = std::move(mat);
					}
				}
				if (const auto* smc = world.TryGet<SkinnedMeshComponent>(e))
				{
					rec.skinned = SkinnedRecord{.clipIndex = smc->clipIndex, .animTime = smc->animTime, .playbackSpeed = smc->playbackSpeed, .looping = smc->looping};
				}
				if (const auto* col = world.TryGet<ColliderComponent>(e))
				{
					PhysicsRecord pr{.shapeType = col->shape, .halfExtents = col->halfExtents, .radius = col->radius, .halfHeight = col->halfHeight};
					pr.center = col->center;
					pr.friction = col->friction;
					pr.restitution = col->restitution;
					pr.isSensor = col->isSensor;
					if (const auto* rb = world.TryGet<RigidBodyComponent>(e))
					{
						pr.motionType = rb->motionType;
						pr.mass = rb->mass;
						pr.linearDamping = rb->linearDamping;
						pr.angularDamping = rb->angularDamping;
						pr.gravityFactor = rb->gravityFactor;
						pr.maxLinearVelocity = rb->maxLinearVelocity;
						pr.maxAngularVelocity = rb->maxAngularVelocity;
						pr.continuousCollision = rb->continuousCollision;
						pr.allowSleeping = rb->allowSleeping;
						pr.lockPosition = rb->lockPosition;
						pr.lockRotation = rb->lockRotation;
					}
					else
					{
						pr.motionType = PhysicsMotionType::Static;
					}
					rec.physics = pr;
				}
				if (const auto* j = world.TryGet<JointComponent>(e))
				{
					JointRecord jr;
					jr.type = j->type;
					if (j->target.IsValid())
					{
						const auto it = indexOf.find(j->target.id);
						jr.targetIndex = it != indexOf.end() ? it->second : -1;
					}
					jr.anchor = j->anchor;
					jr.axis = j->axis;
					jr.minLimit = j->minLimit;
					jr.maxLimit = j->maxLimit;
					jr.distance = j->distance;
					jr.collideConnected = j->collideConnected;
					rec.joint = jr;
				}
				if (const auto* c = world.TryGet<ui::UICanvas>(e))
				{
					rec.uiCanvas = UICanvasRecord{static_cast<std::uint8_t>(c->scaleMode), c->referenceResolution, c->sortBias};
				}
				if (const auto* r = world.TryGet<ui::UIRect>(e))
				{
					rec.uiRect = UIRectRecord{r->anchorMin, r->anchorMax, r->offsetMin, r->offsetMax, r->pivot};
				}
				if (const auto* im = world.TryGet<ui::UIImage>(e))
				{
					UIImageRecord ir;
					ir.color = im->color;
					ir.cornerRadius = im->cornerRadius;
					if (im->texture.IsValid() && im->texture.index != TextureHandle::kBrokenIndex)
					{
						textures.TryGetPath(im->texture, ir.texturePath);
					}
					rec.uiImage = std::move(ir);
				}
				if (const auto* tx = world.TryGet<ui::UIText>(e))
				{
					rec.uiText = UITextRecord{tx->text, tx->fontName, tx->pixelSize, tx->color, static_cast<std::uint8_t>(tx->hAlign), static_cast<std::uint8_t>(tx->vAlign), tx->wrap};
				}
				if (const auto* er = world.TryGet<EffectRefComponent>(e))
				{
					EffectRecord fx;
					fx.name = er->name;
					if (const auto* ep = world.TryGet<EffectParamsComponent>(e))
					{
						fx.params = ep->params;
					}
					rec.effect = std::move(fx);
				}
				if (const auto* bob = world.TryGet<BobComponent>(e))
				{
					BobComponent clean = *bob;
					clean.baseCaptured = false;
					clean.time = 0.0f;
					rec.bob = clean;
				}
				if (const auto* spin = world.TryGet<SpinComponent>(e))
				{
					rec.spin = *spin;
				}
				if (const auto* orbit = world.TryGet<OrbitComponent>(e))
				{
					rec.orbit = *orbit;
				}
				if (const auto* pulse = world.TryGet<MaterialPulseComponent>(e))
				{
					MaterialPulseComponent clean = *pulse;
					clean.time = 0.0f;
					rec.materialPulse = clean;
				}
				if (const auto* scale = world.TryGet<ScalePulseComponent>(e))
				{
					ScalePulseComponent clean = *scale;
					clean.baseCaptured = false;
					clean.time = 0.0f;
					rec.scalePulse = clean;
				}
				if (const auto* look = world.TryGet<LookAtComponent>(e))
				{
					rec.lookAt = *look;
				}
				if (const auto* pl = world.TryGet<PointLightComponent>(e))
				{
					rec.pointLight = *pl;
				}
				if (const auto* sl = world.TryGet<SpotLightComponent>(e))
				{
					rec.spotLight = *sl;
				}
				if (const auto* cam = world.TryGet<CameraComponent>(e))
				{
					rec.camera = *cam;
					rec.mainCamera = world.Has<MainCameraComponent>(e);
				}
				if (const auto* orbit = world.TryGet<OrbitCameraComponent>(e))
				{
					rec.orbitCamera = *orbit;
				}
				if (const auto* script = world.TryGet<ScriptComponent>(e); script != nullptr)
				{
					for (const ScriptEntry& entry: script->scripts)
					{
						if (entry.path.empty())
						{
							continue;
						}
						rec.scripts.push_back(ScriptRecord{.type = entry.path, .properties = ScriptPropsToSceneRefs(entry.properties, indexOf)});
					}
				}
				scene.entities.push_back(std::move(rec));
			}

			std::unordered_set<std::string> seenAssetIds;
			const auto addManifest = [&](const AssetSource& src)
			{
				const std::string idHex = ComputeAssetId(src).ToHex();
				if (!seenAssetIds.insert(idHex).second)
				{
					return;
				}
				scene.assetManifest.push_back(AssetManifestEntry{.id = idHex, .type = (src.type == AssetType::Mesh ? "mesh" : "texture"), .path = src.path, .subIndex = src.subIndex, .builtin = src.builtin});
			};
			for (const EntityRecord& rec: scene.entities)
			{
				if (rec.mesh)
				{
					addManifest(rec.mesh->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(rec.mesh->path) : MakeModelMeshSource(rec.mesh->path, static_cast<int>(rec.mesh->primitiveIndex)));
				}
				if (rec.material)
				{
					for (const std::string& p: {rec.material->albedoPath, rec.material->normalPath, rec.material->metallicRoughnessPath, rec.material->occlusionPath, rec.material->emissivePath})
					{
						if (!p.empty())
						{
							addManifest(MakeTextureSource(p));
						}
					}
				}
				if (rec.uiImage && !rec.uiImage->texturePath.empty())
				{
					addManifest(MakeTextureSource(rec.uiImage->texturePath));
				}
			}
		}
	} // namespace

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer)
	{
		SceneDescription scene;
		scene.kind = world.GetSceneKind();
		scene.features = world.GetSceneFeatures();
		auto& reg = world.GetRegistry();

		// Punctual lights are entities now (LightComponents.hpp) and serialize
		if (renderer != nullptr)
		{
			EnvironmentRecord env;
			env.ambient = renderer->GetAmbientLight();
			env.sunDirection = renderer->GetDirectionalLightDirection();
			env.sunIntensity = renderer->GetDirectionalLightIntensity();
			env.sunColor = renderer->GetSunColor();
			env.skyHorizon = renderer->GetSkyHorizonColor();
			env.skyZenith = renderer->GetSkyZenithColor();
			env.skyVoid = renderer->GetSkyVoidColor();
			scene.environment = env;
		}

		// - excluded so boot auto-generation and Play snapshots never duplicate
		std::vector<Entity> order;
		std::unordered_set<std::uint32_t> seen;
		for (const Entity root: world.Roots())
		{
			AppendCaptureOrder(world, root, order, seen);
		}

		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (!e.IsValid() || ecs::HasSceneTransientAncestor(world, e))
			{
				continue;
			}
			const auto* h = world.TryGet<HierarchyComponent>(e);
			if (!seen.contains(e.id) && (!h || !h->parent.IsValid()))
			{
				AppendCaptureOrder(world, e, order, seen);
			}
		}

		std::unordered_map<std::uint32_t, int> indexOf;
		for (std::size_t i = 0; i < order.size(); ++i)
		{
			indexOf[order[i].id] = static_cast<int>(i);
		}

		AppendEntityRecords(scene, world, order, indexOf, materials, textures);
		return scene;
	}

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		SceneDescription desc;
		std::vector<Entity> order;
		for (const Entity root: roots)
		{
			const std::size_t start = order.size();
			order.push_back(root);
			for (std::size_t i = start; i < order.size(); ++i)
			{
				if (const auto* h = world.TryGet<HierarchyComponent>(order[i]))
				{
					order.insert(order.end(), h->children.begin(), h->children.end());
				}
			}
		}
		std::unordered_map<std::uint32_t, int> indexOf;
		for (std::size_t i = 0; i < order.size(); ++i)
		{
			indexOf[order[i].id] = static_cast<int>(i);
		}
		AppendEntityRecords(desc, world, order, indexOf, materials, textures);
		return desc;
	}

	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		SceneDescription prefab = CaptureSubtrees(world, {root}, materials, textures);
		if (const auto* nc = world.TryGet<NameComponent>(root))
		{
			prefab.name = nc->name;
		}
		return prefab;
	}

	std::string WriteToml(const SceneDescription& scene)
	{
		toml::table root;
		toml::table header;
		header.insert("version", kSceneFormatVersion);
		header.insert("name", scene.name);
		header.insert("kind", SceneKindName(scene.kind));
		header.insert("features", SceneFeaturesToToml(scene.features));
		root.insert("scene", std::move(header));

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

		toml::array entities;
		for (const EntityRecord& rec: scene.entities)
		{
			toml::table t;
			t.insert("name", rec.name);
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
			if (rec.bob)
			{
				t.insert("bob", WriteReflectedToToml("Bob", &*rec.bob));
			}
			if (rec.spin)
			{
				t.insert("spin", WriteReflectedToToml("Spin", &*rec.spin));
			}
			if (rec.orbit)
			{
				t.insert("orbit", WriteReflectedToToml("Orbit", &*rec.orbit));
			}
			if (rec.materialPulse)
			{
				t.insert("material_pulse", WriteReflectedToToml("Material Pulse", &*rec.materialPulse));
			}
			if (rec.scalePulse)
			{
				t.insert("scale_pulse", WriteReflectedToToml("Scale Pulse", &*rec.scalePulse));
			}
			if (rec.lookAt)
			{
				t.insert("look_at", WriteReflectedToToml("Look At", &*rec.lookAt));
			}
			if (rec.pointLight)
			{
				t.insert("point_light", WriteReflectedToToml("Point Light", &*rec.pointLight));
			}
			if (rec.spotLight)
			{
				t.insert("spot_light", WriteReflectedToToml("Spot Light", &*rec.spotLight));
			}
			if (rec.camera)
			{
				toml::table c = WriteReflectedToToml("Camera", &*rec.camera);
				c.insert("main", rec.mainCamera);
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
			entities.push_back(std::move(t));
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

		std::ostringstream out;
		out << "# AetherCore scene - generated by the debug editor\n" << root << "\n";
		return out.str();
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

		const int sourceVersion = static_cast<int>(root["scene"]["version"].value_or(std::int64_t{1}));
		ApplySceneMigrations(root, sourceVersion);

		SceneDescription scene;
		scene.name = root["scene"]["name"].value_or(std::string{});
		scene.version = sourceVersion;
		scene.kind = SceneKindFromName(root["scene"]["kind"].value_or(std::string{"3d"}));
		scene.features = SceneFeaturesFromToml(root["scene"]["features"].as_array(), scene.kind);
		if (scene.version < kSceneFormatVersion)
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
		if (entities == nullptr)
		{
			return scene;
		}

		for (const auto& node: *entities)
		{
			const auto* t = node.as_table();
			if (t == nullptr)
			{
				continue;
			}
			const toml::node_view<const toml::node> tv{*t};

			EntityRecord rec;
			rec.name = tv["name"].value_or(std::string{});
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
			if (const auto* b = tv["bob"].as_table())
			{
				BobComponent c{};
				ReadReflectedFromToml("Bob", *b, &c);
				rec.bob = c;
			}
			if (const auto* s = tv["spin"].as_table())
			{
				SpinComponent c{};
				ReadReflectedFromToml("Spin", *s, &c);
				rec.spin = c;
			}
			if (const auto* o = tv["orbit"].as_table())
			{
				OrbitComponent c{};
				ReadReflectedFromToml("Orbit", *o, &c);
				rec.orbit = c;
			}
			if (const auto* p = tv["material_pulse"].as_table())
			{
				MaterialPulseComponent c{};
				ReadReflectedFromToml("Material Pulse", *p, &c);
				rec.materialPulse = c;
			}
			if (const auto* p = tv["scale_pulse"].as_table())
			{
				ScalePulseComponent c{};
				ReadReflectedFromToml("Scale Pulse", *p, &c);
				rec.scalePulse = c;
			}
			if (const auto* p = tv["look_at"].as_table())
			{
				LookAtComponent c{};
				ReadReflectedFromToml("Look At", *p, &c);
				rec.lookAt = c;
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
			if (const auto* c = tv["camera"].as_table())
			{
				CameraComponent cam{};
				ReadReflectedFromToml("Camera", *c, &cam);
				rec.camera = cam;
				rec.mainCamera = toml::node_view<const toml::node>{*c}["main"].value_or(false);
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
			scene.entities.push_back(std::move(rec));
		}
		return scene;
	}

	namespace
	{
		std::filesystem::path g_projectScenesDirectory;
		std::filesystem::path g_projectPrefabsDirectory;

		constexpr std::string_view kProjectScenesVfsDir = "scenes";
		constexpr std::string_view kProjectPrefabsVfsDir = "assets/prefabs";
		constexpr std::string_view kSceneSuffix = ".scene.toml";
		constexpr std::string_view kPrefabSuffix = ".prefab.toml";

		std::string ProjectVirtualPath(std::string_view directory, const std::string& name, std::string_view suffix)
		{
			std::string path = "project://";
			path += directory;
			path += "/";
			path += name;
			path += suffix;
			return path;
		}

		std::optional<std::string> ReadProjectText(std::string_view directory, const std::string& name, std::string_view suffix)
		{
			if (!io::FileSystem::IsInitialized() || !io::FileSystem::IsMounted("project"))
			{
				return std::nullopt;
			}

			auto text = io::FileSystem::ReadFileText(ProjectVirtualPath(directory, name, suffix));
			if (!text)
			{
				return std::nullopt;
			}
			return std::move(*text);
		}

		std::vector<std::string> ListProjectFiles(std::string_view directory, std::string_view suffix)
		{
			std::vector<std::string> names;
			if (!io::FileSystem::IsInitialized() || !io::FileSystem::IsMounted("project"))
			{
				return names;
			}

			std::string pattern = "project://";
			pattern += directory;
			pattern += "/*";
			pattern += suffix;
			auto matches = io::FileSystem::Glob(pattern);
			if (!matches)
			{
				return names;
			}

			for (const std::string& match: *matches)
			{
				const std::string file = std::filesystem::path(match).filename().generic_string();
				if (file.size() > suffix.size() && file.ends_with(suffix))
				{
					names.push_back(file.substr(0, file.size() - suffix.size()));
				}
			}
			std::sort(names.begin(), names.end());
			return names;
		}
	} // namespace

	void SetProjectSceneDirectories(std::filesystem::path scenesDir, std::filesystem::path prefabsDir)
	{
		g_projectScenesDirectory = std::move(scenesDir);
		g_projectPrefabsDirectory = std::move(prefabsDir);
	}

	void ClearProjectSceneDirectories()
	{
		g_projectScenesDirectory.clear();
		g_projectPrefabsDirectory.clear();
	}

	std::string ScenesDirectory()
	{
		if (!g_projectScenesDirectory.empty())
		{
			return g_projectScenesDirectory.string();
		}
#ifdef AETHER_SCENES_SOURCE_DIR
		return AETHER_SCENES_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("scenes").string();
#endif
	}

	std::string PrefabsDirectory()
	{
		if (!g_projectPrefabsDirectory.empty())
		{
			return g_projectPrefabsDirectory.string();
		}
#ifdef AETHER_PREFABS_SOURCE_DIR
		return AETHER_PREFABS_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("prefabs").string();
#endif
	}

	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefab)
	{
		const std::filesystem::path dir{PrefabsDirectory()};
		if (!io::file_util::CreateDirectories(dir))
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot create directory '{}'", dir.string());
			return false;
		}
		const std::filesystem::path path = dir / (prefabName + ".prefab.toml");

		if (!io::file_util::WriteText(path, WriteToml(prefab)))
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot write '{}'", path.string());
			return false;
		}
		AE_INFO(LogCategory::App, "Prefab saved: {} ({} entities)", path.string(), prefab.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName)
	{
		if (auto text = ReadProjectText(kProjectPrefabsVfsDir, prefabName, kPrefabSuffix))
		{
			return ParseToml(*text);
		}

		const std::filesystem::path path = std::filesystem::path{PrefabsDirectory()} / (prefabName + ".prefab.toml");
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			AE_WARN(LogCategory::App, "ReadPrefabFile: cannot read '{}'", path.string());
			return std::nullopt;
		}
		return ParseToml(*text);
	}

	std::vector<std::string> ListPrefabFiles()
	{
		std::vector<std::string> names = ListProjectFiles(kProjectPrefabsVfsDir, kPrefabSuffix);
		if (!names.empty())
		{
			return names;
		}

		const std::filesystem::path dir{PrefabsDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			const std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = kPrefabSuffix;
			if (file.size() > kSuffix.size() && file.ends_with(kSuffix))
			{
				names.push_back(file.substr(0, file.size() - kSuffix.size()));
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene)
	{
		const std::filesystem::path dir{ScenesDirectory()};
		if (!io::file_util::CreateDirectories(dir))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot create directory '{}'", dir.string());
			return false;
		}
		const std::filesystem::path path = dir / (sceneName + ".scene.toml");

		if (!io::file_util::WriteText(path, WriteToml(scene)))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot write '{}'", path.string());
			return false;
		}
		AE_INFO(LogCategory::App, "Scene saved: {} ({} entities)", path.string(), scene.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName)
	{
		if (auto text = ReadProjectText(kProjectScenesVfsDir, sceneName, kSceneSuffix))
		{
			return ParseToml(*text);
		}

		const std::filesystem::path path = std::filesystem::path{ScenesDirectory()} / (sceneName + ".scene.toml");
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			AE_WARN(LogCategory::App, "ReadSceneFile: cannot read '{}'", path.string());
			return std::nullopt;
		}
		return ParseToml(*text);
	}

	std::vector<std::string> ListSceneFiles()
	{
		std::vector<std::string> names = ListProjectFiles(kProjectScenesVfsDir, kSceneSuffix);
		if (!names.empty())
		{
			return names;
		}

		const std::filesystem::path dir{ScenesDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			const std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = kSceneSuffix;
			if (file.size() > kSuffix.size() && file.ends_with(kSuffix))
			{
				names.push_back(file.substr(0, file.size() - kSuffix.size()));
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	ApplySceneDeps MakeApplySceneDeps(ServiceContainer& services)
	{
		auto* sceneCtx = services.TryGet<scripting::SceneContext>();
		auto* assets = services.TryGet<AssetManager>();
		ApplySceneDeps deps{};
		deps.assets = assets;
		deps.primitives = services.TryGet<PrimitiveMeshes>();
		deps.effectManager = sceneCtx != nullptr ? sceneCtx->effects : nullptr;
		deps.effectParams = services.TryGet<EffectParamBuffer>();
		deps.pipelines = assets != nullptr ? &assets->GetPipelineCache() : nullptr;
		deps.sceneContext = sceneCtx;
		deps.physics = services.TryGet<PhysicsSystem>();
		deps.renderer = services.TryGet<Renderer>();
		deps.assetDatabase = services.TryGet<AssetDatabase>();
		if (const auto* bakeHook = services.TryGet<ModelBakeHook>(); bakeHook != nullptr)
		{
			deps.ensureModelBaked = bakeHook->ensureBaked;
		}
		return deps;
	}

	namespace
	{
		template<typename T>
		void RemoveIf(World& world, Entity entity)
		{
			if (world.Has<T>(entity))
			{
				world.Remove<T>(entity);
			}
		}

		void ClearTags(World& world, Entity entity)
		{
			ForEachTag(
			        [&](const std::string&, std::uint32_t tagId)
			        {
				        if (TagHas(&world, entity.id, tagId))
				        {
					        TagRemove(&world, entity.id, tagId);
				        }
			        });
		}

		void ResetRestorableEntity(World& world, Entity entity)
		{
			ecs::DetachFromParent(world, entity);
			ClearTags(world, entity);

			RemoveIf<NameComponent>(world, entity);
			RemoveIf<TransformComponent>(world, entity);
			RemoveIf<HierarchyComponent>(world, entity);
			RemoveIf<MeshComponent>(world, entity);
			RemoveIf<MeshSourceComponent>(world, entity);
			RemoveIf<MaterialComponent>(world, entity);
			RemoveIf<MaterialInstanceComponent>(world, entity);
			RemoveIf<SkinnedMeshComponent>(world, entity);
			RemoveIf<JointComponent>(world, entity);
			RemoveIf<CollisionEventsComponent>(world, entity);
			RemoveIf<ColliderComponent>(world, entity);
			RemoveIf<RigidBodyComponent>(world, entity);
			RemoveIf<PhysicsStateComponent>(world, entity);
			RemoveIf<ui::UICanvas>(world, entity);
			RemoveIf<ui::UIRect>(world, entity);
			RemoveIf<ui::UIImage>(world, entity);
			RemoveIf<ui::UIText>(world, entity);
			RemoveIf<EffectRefComponent>(world, entity);
			RemoveIf<EffectParamsComponent>(world, entity);
			RemoveIf<BobComponent>(world, entity);
			RemoveIf<SpinComponent>(world, entity);
			RemoveIf<OrbitComponent>(world, entity);
			RemoveIf<MaterialPulseComponent>(world, entity);
			RemoveIf<ScalePulseComponent>(world, entity);
			RemoveIf<LookAtComponent>(world, entity);
			RemoveIf<PointLightComponent>(world, entity);
			RemoveIf<SpotLightComponent>(world, entity);
			RemoveIf<CameraComponent>(world, entity);
			RemoveIf<OrbitCameraComponent>(world, entity);
			RemoveIf<MainCameraComponent>(world, entity);
			RemoveIf<ScriptComponent>(world, entity);
			RemoveIf<MeshRendererComponent>(world, entity);
			RemoveIf<SpriteRendererComponent>(world, entity);
			RemoveIf<DisabledComponent>(world, entity);
		}

		std::vector<Entity> ApplySceneToEntities(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities)
		{
			if (deps.assetDatabase != nullptr)
			{
				for (const AssetManifestEntry& a: scene.assetManifest)
				{
					AssetSource src;
					src.type = (a.type == "mesh") ? AssetType::Mesh : AssetType::Texture;
					src.path = a.path;
					src.subIndex = a.subIndex;
					src.builtin = a.builtin;
					deps.assetDatabase->Register(src);
				}
			}

			if (deps.renderer != nullptr && scene.environment)
			{
				const EnvironmentRecord& env = *scene.environment;
				deps.renderer->SetAmbientLight(env.ambient);
				deps.renderer->SetDirectionalLight(env.sunDirection, env.sunIntensity);
				deps.renderer->SetSunColor(env.sunColor);
				deps.renderer->SetSkyGradient(env.skyHorizon, env.skyZenith);
				deps.renderer->SetSkyVoidColor(env.skyVoid);
			}

			std::size_t behaviorCount = 0;
			std::size_t effectCount = 0;

			for (std::size_t i = 0; i < scene.entities.size(); ++i)
			{
				const EntityRecord& rec = scene.entities[i];
				const Entity e = created[i];

				if (!rec.name.empty())
				{
					world.Emplace<NameComponent>(e, NameComponent{.name = rec.name});
				}
				for (const std::string& tag: rec.tags)
				{
					const std::uint32_t tagId = TagCreate(tag);
					if (tagId != UINT32_MAX)
					{
						TagAdd(&world, e.id, tagId);
					}
				}
				if (rec.disabled)
				{
					world.EmplaceOrReplace<DisabledComponent>(e);
				}
				if (rec.sprite)
				{
					world.EmplaceOrReplace<SpriteRendererComponent>(e, *rec.sprite);
				}
				if (rec.meshRenderer)
				{
					world.EmplaceOrReplace<MeshRendererComponent>(e, MeshRendererComponent{.visible = rec.meshRendererVisible, .castShadows = rec.meshRendererCastShadows});
				}
				if (rec.hasTransform)
				{
					world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(rec.position, rec.eulerDeg, rec.scale)});
				}

				if (rec.physics)
				{
					const PhysicsRecord& phys = *rec.physics;
					world.Emplace<ColliderComponent>(e,
					        ColliderComponent{
					                .shape = phys.shapeType,
					                .halfExtents = phys.halfExtents,
					                .radius = phys.radius,
					                .halfHeight = phys.halfHeight,
					                .center = phys.center,
					                .friction = phys.friction,
					                .restitution = phys.restitution,
					                .isSensor = phys.isSensor,
					                .layer = phys.isSensor ? PhysicsLayer::Sensor : PhysicsLayer::Moving,
					        });
					world.Emplace<RigidBodyComponent>(e,
					        RigidBodyComponent{
					                .motionType = phys.motionType,
					                .mass = phys.mass,
					                .linearDamping = phys.linearDamping,
					                .angularDamping = phys.angularDamping,
					                .gravityFactor = phys.gravityFactor,
					                .maxLinearVelocity = phys.maxLinearVelocity,
					                .maxAngularVelocity = phys.maxAngularVelocity,
					                .continuousCollision = phys.continuousCollision,
					                .allowSleeping = phys.allowSleeping,
					                .lockPosition = phys.lockPosition,
					                .lockRotation = phys.lockRotation,
					        });
				}
				if (rec.joint)
				{
					const JointRecord& jr = *rec.joint;
					Entity targetEntity{};
					if (jr.targetIndex >= 0 && jr.targetIndex < static_cast<int>(created.size()))
					{
						targetEntity = created[static_cast<std::size_t>(jr.targetIndex)];
					}
					world.Emplace<JointComponent>(e,
					        JointComponent{
					                .type = jr.type,
					                .target = targetEntity,
					                .anchor = jr.anchor,
					                .axis = jr.axis,
					                .minLimit = jr.minLimit,
					                .maxLimit = jr.maxLimit,
					                .distance = jr.distance,
					                .collideConnected = jr.collideConnected,
					        });
				}

				if (rec.uiCanvas)
				{
					world.Emplace<ui::UICanvas>(e, ui::UICanvas{static_cast<ui::UICanvas::ScaleMode>(rec.uiCanvas->scaleMode), rec.uiCanvas->referenceResolution, rec.uiCanvas->sortBias});
				}
				if (rec.uiRect)
				{
					world.Emplace<ui::UIRect>(e, ui::UIRect{rec.uiRect->anchorMin, rec.uiRect->anchorMax, rec.uiRect->offsetMin, rec.uiRect->offsetMax, rec.uiRect->pivot, glm::vec4{0.f}});
				}
				if (rec.uiImage)
				{
					ui::UIImage im;
					im.color = rec.uiImage->color;
					im.cornerRadius = rec.uiImage->cornerRadius;
					if (!rec.uiImage->texturePath.empty() && deps.assets != nullptr)
					{
						im.texture = deps.assets->GetTextureRegistry().Acquire(rec.uiImage->texturePath);
						if (deps.assetDatabase != nullptr)
						{
							deps.assetDatabase->Register(MakeTextureSource(rec.uiImage->texturePath));
						}
					}
					world.Emplace<ui::UIImage>(e, im);
				}
				if (rec.uiText)
				{
					world.Emplace<ui::UIText>(
					        e, ui::UIText{rec.uiText->text, rec.uiText->fontName, rec.uiText->pixelSize, rec.uiText->color, static_cast<ui::UIText::HAlign>(rec.uiText->hAlign), static_cast<ui::UIText::VAlign>(rec.uiText->vAlign), rec.uiText->wrap});
				}

				if (rec.mesh)
				{
					const Mesh* resolved = nullptr;
					LoadedModel* model = nullptr;
					if (rec.mesh->kind == MeshSourceComponent::Kind::Primitive)
					{
						if (deps.assets != nullptr && deps.primitives != nullptr)
						{
							if (const auto prim = PrimitiveFromName(rec.mesh->path))
							{
								resolved = &deps.primitives->Get(*prim);
							}
						}
					}
					else if (deps.sceneContext != nullptr)
					{
						auto& ctx = *deps.sceneContext;
						if (const auto it = ctx.loadedModelMap.find(rec.mesh->path); it != ctx.loadedModelMap.end())
						{
							model = &ctx.loadedModels[it->second];
						}
						else if (deps.assets != nullptr)
						{
							auto result = deps.assets->LoadModel(rec.mesh->path);
							if (!result && deps.ensureModelBaked)
							{
								std::string bakeError;
								if (deps.ensureModelBaked(rec.mesh->path, bakeError))
								{
									AE_INFO(LogCategory::App, "Scene load: auto-imported model '{}'", rec.mesh->path);
									result = deps.assets->LoadModel(rec.mesh->path);
								}
								else
								{
									AE_WARN(LogCategory::App, "Scene load: auto-import of model '{}' failed: {}", rec.mesh->path, bakeError);
								}
							}
							if (result)
							{
								ctx.loadedModels.push_back(std::move(result.value()));
								ctx.loadedModelMap[rec.mesh->path] = ctx.loadedModels.size() - 1;
								model = &ctx.loadedModels.back();
							}
							else
							{
								AE_WARN(LogCategory::App, "Scene load: model '{}' failed: {}", rec.mesh->path, result.error());
							}
						}
						if (model != nullptr && rec.mesh->primitiveIndex < model->primitives.size())
						{
							resolved = &model->primitives[rec.mesh->primitiveIndex].mesh;
						}
					}

					if (resolved != nullptr)
					{
						world.Emplace<MeshComponent>(e, MeshComponent{.mesh = resolved});
						world.Emplace<MeshSourceComponent>(e, *rec.mesh);
						if (deps.assetDatabase != nullptr)
						{
							deps.assetDatabase->Register(rec.mesh->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(rec.mesh->path) : MakeModelMeshSource(rec.mesh->path, static_cast<int>(rec.mesh->primitiveIndex)));
						}
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: mesh source '{}' unresolved for '{}'", rec.mesh->path, rec.name);
					}

					if (rec.skinned && model != nullptr && model->animationDb.IsValid())
					{
						const auto& primitive = model->primitives[rec.mesh->primitiveIndex];
						if (primitive.skinIndex >= 0)
						{
							const auto skinIdx = static_cast<std::uint32_t>(primitive.skinIndex);
							const std::uint32_t joints = model->animationDb.GetSkinJointCount(skinIdx);
							const std::uint32_t clipCount = model->animationDb.GetClipCount();
							SkinnedMeshComponent smc{};
							smc.animDb = &model->animationDb;
							smc.skinIndex = skinIdx;
							smc.jointCount = joints;
							smc.clipIndex = clipCount > 0 ? std::min(rec.skinned->clipIndex, clipCount - 1) : 0;
							smc.animTime = rec.skinned->animTime;
							smc.playbackSpeed = rec.skinned->playbackSpeed;
							smc.looping = rec.skinned->looping;
							world.EmplaceOrReplace<SkinnedMeshComponent>(e, smc);
						}
					}
				}

				if (rec.material && deps.assets != nullptr)
				{
					MaterialAsset asset = rec.material->asset;
					TextureRegistry& textures = deps.assets->GetTextureRegistry();
					const auto acquire = [&textures](const std::string& path, TextureHandle& out)
					{
						out = path.empty() ? TextureHandle{} : textures.Acquire(path);
					};
					acquire(rec.material->albedoPath, asset.albedoTex);
					acquire(rec.material->normalPath, asset.normalTex);
					acquire(rec.material->metallicRoughnessPath, asset.metallicRoughnessTex);
					acquire(rec.material->occlusionPath, asset.occlusionTex);
					acquire(rec.material->emissivePath, asset.emissiveTex);
					if (deps.assetDatabase != nullptr)
					{
						for (const std::string& texPath: {rec.material->albedoPath, rec.material->normalPath, rec.material->metallicRoughnessPath, rec.material->occlusionPath, rec.material->emissivePath})
						{
							if (!texPath.empty())
							{
								deps.assetDatabase->Register(MakeTextureSource(texPath));
							}
						}
					}
					MaterialSystem::AssignMaterial(world, e, deps.assets->GetMaterialRegistry(), deps.assets->GetPipelineCache(), asset);
					for (const TextureHandle h: {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex})
					{
						if (h.IsValid())
						{
							textures.Release(h);
						}
					}
				}

				if (rec.effect && !rec.effect->name.empty() && deps.effectManager != nullptr && deps.effectParams != nullptr && deps.pipelines != nullptr)
				{
					if (effects::ApplyEntityEffect(world, e, rec.effect->name, *deps.effectManager, *deps.pipelines, *deps.effectParams, &rec.effect->params))
					{
						++effectCount;
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: unknown effect '{}'", rec.effect->name);
					}
				}
				else if (rec.effect)
				{
					AE_WARN(LogCategory::App, "Scene load: effect '{}' on '{}' skipped (missing effect deps)", rec.effect->name, rec.name);
				}

				if (rec.bob)
				{
					world.Emplace<BobComponent>(e, *rec.bob);
					++behaviorCount;
				}
				if (rec.spin)
				{
					world.Emplace<SpinComponent>(e, *rec.spin);
					++behaviorCount;
				}
				if (rec.orbit)
				{
					world.Emplace<OrbitComponent>(e, *rec.orbit);
					++behaviorCount;
				}
				if (rec.materialPulse)
				{
					world.Emplace<MaterialPulseComponent>(e, *rec.materialPulse);
					++behaviorCount;
				}
				if (rec.scalePulse)
				{
					world.Emplace<ScalePulseComponent>(e, *rec.scalePulse);
					++behaviorCount;
				}
				if (rec.lookAt)
				{
					world.Emplace<LookAtComponent>(e, *rec.lookAt);
					++behaviorCount;
				}
				if (rec.pointLight)
				{
					world.Emplace<PointLightComponent>(e, *rec.pointLight);
				}
				if (rec.spotLight)
				{
					world.Emplace<SpotLightComponent>(e, *rec.spotLight);
				}
				if (rec.camera)
				{
					world.Emplace<CameraComponent>(e, *rec.camera);
					if (rec.mainCamera)
					{
						ecs::SetMainCameraEntity(world, e);
					}
				}
				if (rec.orbitCamera)
				{
					world.EmplaceOrReplace<OrbitCameraComponent>(e, *rec.orbitCamera);
				}
				if (!rec.scripts.empty())
				{
					ScriptComponent component;
					component.scripts.reserve(rec.scripts.size());
					for (const ScriptRecord& script: rec.scripts)
					{
						component.scripts.push_back(ScriptEntry{.path = script.type, .attached = false, .properties = ScriptPropsFromSceneRefs(script.properties, created)});
					}
					world.Emplace<ScriptComponent>(e, std::move(component));
				}
			}

			std::vector<Entity> migratedLights;
			for (const LightRecord& light: scene.lights)
			{
				if (light.isSpot)
				{
					migratedLights.push_back(ecs::CreateSpotLightEntity(world,
					        light.position,
					        light.direction,
					        SpotLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .innerAngleRad = light.innerAngleRad, .outerAngleRad = light.outerAngleRad, .castsShadow = light.castsShadow}));
				}
				else
				{
					migratedLights.push_back(ecs::CreatePointLightEntity(world, light.position, PointLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .castsShadow = light.castsShadow}));
				}
			}
			if (!migratedLights.empty())
			{
				AE_INFO(LogCategory::App, "Scene load: migrated {} legacy light record(s) to light entities - re-save to upgrade the file", migratedLights.size());
			}

			for (std::size_t i = 0; i < scene.entities.size(); ++i)
			{
				const int parent = scene.entities[i].parentIndex;
				if (parent >= 0 && static_cast<std::size_t>(parent) < created.size())
				{
					ecs::SetParent(world, created[i], created[static_cast<std::size_t>(parent)]);
				}
			}

			if (deps.sceneContext != nullptr && registerSceneEntities)
			{
				for (const Entity e: created)
				{
					deps.sceneContext->sceneEntities.push_back(e);
				}
				for (const Entity e: migratedLights)
				{
					deps.sceneContext->sceneEntities.push_back(e);
				}
			}
			AE_INFO(LogCategory::App, "Scene apply: {} entities, {} behaviors, {} effects (format v{})", created.size() + migratedLights.size(), behaviorCount, effectCount, scene.version);
			return created;
		}
	} // namespace

	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		std::vector<Entity> created;
		created.reserve(scene.entities.size());
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			created.push_back(world.Create());
		}
		return ApplySceneToEntities(scene, world, deps, std::move(created), true);
	}

	std::vector<Entity> RestoreSceneInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		if (deps.physics != nullptr)
		{
			deps.physics->WaitForStepIdle();
		}

		auto& reg = world.GetRegistry();
		std::vector<Entity> targets;
		targets.reserve(scene.entities.size());
		std::unordered_set<std::uint32_t> restoredIds;
		for (const EntityRecord& rec: scene.entities)
		{
			Entity target{rec.entityId};
			if (!target.IsValid() || !reg.valid(World::ToEntt(target)))
			{
				target = world.Create();
			}
			restoredIds.insert(target.id);
			targets.push_back(target);
		}

		std::vector<Entity> doomed;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (e.IsValid() && !restoredIds.contains(e.id))
			{
				doomed.push_back(e);
			}
		}
		for (const Entity e: doomed)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				ecs::DetachFromParent(world, e);
				world.Destroy(e);
			}
		}

		for (const Entity e: targets)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				ResetRestorableEntity(world, e);
			}
		}

		if (deps.sceneContext != nullptr)
		{
			deps.sceneContext->sceneEntities.clear();
		}
		std::vector<Entity> restored = targets;
		ApplySceneToEntities(scene, world, deps, std::move(targets), true);
		return restored;
	}

	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		// removal must not race the async physics step.
		if (deps.physics != nullptr)
		{
			deps.physics->WaitForStepIdle();
		}
		auto& reg = world.GetRegistry();
		std::vector<Entity> doomed;
		std::vector<Entity> spared;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (reg.valid(handle))
			{
				const Entity e = World::FromEntt(handle);
				if (!e.IsValid())
				{
					continue;
				}
				// owned actors like the player), so replace-all must leave
				if (ecs::HasSceneTransientAncestor(world, e))
				{
					spared.push_back(e);
				}
				else
				{
					doomed.push_back(e);
				}
			}
		}
		for (const Entity e: doomed)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				world.Destroy(e);
			}
		}
		if (deps.sceneContext != nullptr)
		{
			deps.sceneContext->sceneEntities.clear();
			for (const Entity e: spared)
			{
				deps.sceneContext->sceneEntities.push_back(e);
			}
		}

		ApplyScene(scene, world, deps);
	}

	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps)
	{
		const auto scene = ReadSceneFile(sceneName);
		if (!scene)
		{
			return false;
		}
		ReplaceScene(*scene, world, deps);
		AE_INFO(LogCategory::App, "Scene loaded: {} ({} entities)", sceneName, scene->entities.size());
		return true;
	}

	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld)
	{
		const std::vector<Entity> created = ApplyScene(prefab, world, deps);

		for (std::size_t i = 0; i < prefab.entities.size() && i < created.size(); ++i)
		{
			if (prefab.entities[i].parentIndex < 0)
			{
				ecs::SetWorldTransform(world, created[i], localToWorld);
				return created[i];
			}
		}
		return created.empty() ? Entity{} : created.front();
	}
} // namespace aether::app::scene
