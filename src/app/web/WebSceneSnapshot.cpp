#include "WebScene.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "scene/Hierarchy.hpp"
#include "scene/GrassComponent.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/WorldStreaming.hpp"
#include "scene/SceneStreamingReferences.hpp"
#include "ui/UiComponents.hpp"

namespace aether::app::web
{
	namespace
	{
		AetherError SnapshotError(const char* message)
		{
			return AetherError::Engine(message);
		}

		bool Promoted(World& world, Entity entity)
		{
			const auto parent = [&](Entity value) -> Entity
			{
				if (!value.IsValid() || !world.GetRegistry().valid(World::ToEntt(value)))
				{
					return {};
				}
				const auto* hierarchy = world.TryGet<HierarchyComponent>(value);
				return hierarchy ? hierarchy->parent : Entity{};
			};
			Entity slow = entity, fast = entity;
			while (entity.IsValid())
			{
				if (!world.GetRegistry().valid(World::ToEntt(entity)))
				{
					return true;
				}
				if (world.Has<DontDestroyOnLoadComponent>(entity))
				{
					return true;
				}
				entity = parent(entity);
				slow = parent(slow);
				fast = parent(parent(fast));
				if (slow.IsValid() && slow == fast)
				{
					return true;
				}
			}
			return false;
		}

	} // namespace

	Expected<RegionSnapshotStorage> WebScene::MeasureRegionSnapshotStorage(const WebRegion& region) const
	{
		if (region.id.empty())
		{
			return std::unexpected(SnapshotError("Browser streaming region has no identity"));
		}
		RegionSnapshotStorage total;
		bool valid = true;
		const auto add = [&](std::uint64_t& target, std::uint64_t value)
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - target)
			{
				valid = false;
				return;
			}
			target += value;
		};
		const auto multiply = [&](std::uint64_t a, std::uint64_t b) -> std::uint64_t
		{
			if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
			{
				valid = false;
				return 0;
			}
			return a * b;
		};
		const auto string = [&](std::string_view value)
		{
			add(total.stringBytes, static_cast<std::uint64_t>(value.size()));
			add(total.stringBytes, 1);
		};
		const auto fieldStorage = [&](const reflect::FieldDesc& field, const void* component)
		{
			std::size_t bytes = 0, rows = 0;
			if (!field.measureStorage || !field.measureStorage(component, bytes, rows))
			{
				valid = false;
				return;
			}
			add(total.stringBytes, bytes);
			string(field.name);
			if (!field.meta.serializeName.empty())
			{
				string(field.meta.serializeName);
			}
			if (field.meta.enumTable)
			{
				for (const auto& [name, value]: field.meta.enumTable->values)
				{
					string(name);
				}
			}
			for (const auto& element: field.elementFields)
			{
				string(element.name);
			}
			add(total.recordBytes, sizeof(std::pair<std::string, reflect::FieldValue>));
			add(total.recordBytes, multiply(rows, sizeof(std::vector<reflect::FieldValue>)));
			const auto values = multiply(rows, field.elementFields.size());
			add(total.recordBytes, multiply(values, sizeof(reflect::FieldValue)));
			add(total.logicalNodes, sizeof(reflect::FieldValue));
			add(total.logicalNodes, multiply(values, sizeof(reflect::FieldValue)));
			add(total.logicalNodes, rows);
		};
		const auto measure = [&](Entity entity)
		{
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				return;
			}
			add(total.entityCount, 1);
			add(total.recordBytes, sizeof(scene::EntityRecord));
			// Capture's entity vector and lookup/set entries.
			add(total.recordBytes, sizeof(Entity) + sizeof(std::uint32_t) + sizeof(std::uint64_t) + sizeof(std::pair<const std::uint32_t, int>));
			// Each scalar contributes at most one logical TOML node; sizeof the
			// fixed record conservatively dominates its bespoke scalar schema.
			add(total.logicalNodes, sizeof(scene::EntityRecord));
			if (const auto* name = world.TryGet<NameComponent>(entity))
			{
				string(name->name);
			}
			ForEachTag(
			        [&](const std::string& name, std::uint32_t tag)
			        {
				        if (!TagHas(&world, entity.id, tag))
				        {
					        return;
				        }
				        string(name);
				        add(total.recordBytes, sizeof(std::string));
				        add(total.logicalNodes, 1);
			        });
			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(entity))
			{
				string(sprite->texturePath);
				string(sprite->atlasPath);
			}
			if (const auto* animator = world.TryGet<SpriteAnimatorComponent>(entity))
			{
				string(animator->animationPath);
			}
			if (const auto* mesh = world.TryGet<MeshSourceComponent>(entity))
			{
				string(mesh->path);
				string(mesh->primitiveKey);
			}
			if (world.Has<MeshSourceComponent>(entity) || world.Has<GrassComponent>(entity))
			{
				auto material = m_models.MeasureSnapshotMaterialPaths(world, entity);
				if (!material)
				{
					valid = false;
					return;
				}
				add(total.stringBytes, *material);
			}
			if (const auto* image = world.TryGet<ui::UIImage>(entity))
			{
				string(image->texturePath);
			}
			if (const auto* effect = world.TryGet<EffectRefComponent>(entity))
			{
				string(effect->name);
			}
			if (const auto* collider = world.TryGet<ColliderComponent>(entity))
			{
				string(collider->meshSource);
			}
			if (const auto* camera = world.TryGet<CameraComponent>(entity))
			{
				add(total.recordBytes, multiply(camera->gradientStops.size(), sizeof(GradientStop)));
				if (const auto* type = reflect::FindComponentType("Camera"))
				{
					for (const auto& field: type->fields)
					{
						if (field.meta.serialize && field.type == reflect::FieldType::List)
						{
							fieldStorage(field, camera);
						}
					}
				}
			}
			for (const auto& type: reflect::ComponentTypes())
			{
				if (!type.genericSerialize || !type.tryGetRawConst)
				{
					continue;
				}
				const void* component = type.tryGetRawConst(world, entity);
				if (!component)
				{
					continue;
				}
				add(total.recordBytes, sizeof(scene::GenericComponent));
				string(type.name);
				string(type.serializeKey.empty() ? std::string_view(type.name) : std::string_view(type.serializeKey));
				add(total.logicalNodes, 1);
				for (const auto& field: type.fields)
				{
					fieldStorage(field, component);
				}
			}
			if (const auto* joints = world.TryGet<ScriptJointsComponent>(entity))
			{
				add(total.recordBytes, multiply(joints->joints.size(), sizeof(scene::ScriptJointRecord)));
				add(total.logicalNodes, multiply(joints->joints.size(), sizeof(scene::ScriptJointRecord)));
			}
			if (const auto* scripts = world.TryGet<ScriptComponent>(entity))
			{
				for (const auto& script: scripts->scripts)
				{
					string(script.path);
					add(total.recordBytes, sizeof(scene::ScriptRecord));
					add(total.logicalNodes, sizeof(scene::ScriptRecord));
					for (const auto& [name, value]: script.properties)
					{
						// ScriptPropsToSceneRefs makes a temporary map before moving it into the record.
						string(name);
						string(name);
						string(name);
						string(value.str);
						string(value.str);
						add(total.recordBytes, 2 * sizeof(std::pair<const std::string, ScriptPropertyValue>));
						add(total.recordBytes, sizeof(std::pair<const std::string, std::uint64_t>));
						add(total.logicalNodes, sizeof(ScriptPropertyValue) + sizeof(std::uint64_t));
					}
				}
			}
		};
		// Deliberate overcount of duplicates and promoted actors avoids allocating a
		// dedup set or walking ancestry during this preflight.
		for (const auto entity: region.entities)
		{
			measure(entity);
		}
		for (auto&& [handle, owner]: world.View<scene::StreamingRegionOwner>().each())
		{
			if (owner.region == region.id)
			{
				measure(World::FromEntt(handle));
			}
		}
		for (auto&& [handle, node]: world.View<SceneNodeComponent>().each())
		{
			if (node.id)
			{
				add(total.recordBytes, sizeof(std::uint64_t));
			}
		}
		if (!valid)
		{
			return std::unexpected(SnapshotError("Cannot bound browser streaming scene snapshot"));
		}
		return total;
	}

	Expected<scene::SceneDescription> WebScene::CaptureRegionDocument(const WebRegion& region)
	{
		if (region.id.empty())
		{
			return std::unexpected(SnapshotError("Browser streaming region has no identity"));
		}
		std::vector<Entity> entities;
		std::unordered_set<std::uint32_t> seen;
		const auto add = [&](Entity entity)
		{
			if (entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)) && !Promoted(world, entity) && seen.insert(entity.id).second)
			{
				entities.push_back(entity);
			}
		};
		for (Entity entity: region.entities)
		{
			add(entity);
		}
		for (auto&& [handle, owner]: world.View<scene::StreamingRegionOwner>().each())
		{
			if (owner.region == region.id)
			{
				add(World::FromEntt(handle));
			}
		}
		if (entities.size() > 65536)
		{
			return std::unexpected(SnapshotError("Browser streaming region has too many entities"));
		}

		std::unordered_map<std::uint32_t, int> indices;
		std::unordered_set<std::uint64_t> nodes;
		for (std::size_t i = 0; i < entities.size(); ++i)
		{
			const auto entity = entities[i];
			auto* node = world.TryGet<SceneNodeComponent>(entity);
			if (!node)
			{
				node = &world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{scene::GenerateSceneNodeId()});
			}
			if (!node->id)
			{
				node->id = scene::GenerateSceneNodeId();
			}
			if (!nodes.insert(node->id).second)
			{
				return std::unexpected(SnapshotError("Duplicate browser streaming node"));
			}
			indices.emplace(entity.id, static_cast<int>(i));
		}
		scene::SceneDescription document;
		document.kind = world.GetSceneKind();
		document.features = world.GetSceneFeatures();
		document.entities.reserve(entities.size());
		const auto targetIndex = [&](Entity target) -> int
		{
			if (!target.IsValid())
			{
				return -1;
			}
			const auto it = indices.find(target.id);
			return it == indices.end() ? -2 : it->second;
		};
		scene::StreamingNodeIndex referenceIndex(world);
		for (Entity entity: entities)
		{
			scene::EntityRecord record;
			record.nodeId = world.Get<SceneNodeComponent>(entity).id;
			record.sourceNodeId = world.Get<SceneNodeComponent>(entity).sourceId;
			if (const auto* name = world.TryGet<NameComponent>(entity))
			{
				record.name = name->name;
			}
			ForEachTag(
			        [&](const std::string& name, std::uint32_t id)
			        {
				        if (TagHas(&world, entity.id, id))
				        {
					        record.tags.push_back(name);
				        }
			        });
			record.disabled = world.Has<DisabledComponent>(entity);
			if (const auto* transform = world.TryGet<TransformComponent>(entity))
			{
				record.hasTransform = true;
				DecomposeTRS(transform->localToWorld, record.position, record.eulerDeg, record.scale);
			}
			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity); hierarchy && hierarchy->parent.IsValid())
			{
				record.parentIndex = targetIndex(hierarchy->parent);
				if (record.parentIndex == -2)
				{
					return std::unexpected(SnapshotError("Browser streaming parent lies outside region"));
				}
			}
			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(entity))
			{
				record.sprite = *sprite;
			}
			if (const auto* animator = world.TryGet<SpriteAnimatorComponent>(entity))
			{
				record.spriteAnimator = *animator;
			}
			if (const auto* mesh = world.TryGet<MeshSourceComponent>(entity))
			{
				record.mesh = *mesh;
			}
			if (const auto* renderer = world.TryGet<MeshRendererComponent>(entity))
			{
				record.meshRenderer = true;
				record.meshRendererVisible = renderer->visible;
				record.meshRendererCastShadows = renderer->castShadows;
			}
			if (const auto* skin = world.TryGet<SkinnedMeshComponent>(entity))
			{
				record.skinned = scene::SkinnedRecord{.clipIndex = skin->clipIndex, .animTime = skin->animTime, .playbackSpeed = skin->playbackSpeed, .looping = skin->looping};
			}
			if (record.mesh || world.Has<GrassComponent>(entity))
			{
				auto captured = m_models.CaptureSnapshotMaterial(world, entity, record);
				if (!captured)
				{
					return std::unexpected(captured.error());
				}
			}
			if (const auto* camera = world.TryGet<CameraComponent>(entity))
			{
				record.camera = *camera;
				record.mainCamera = world.Has<MainCameraComponent>(entity);
			}
			if (const auto* effect = world.TryGet<EffectRefComponent>(entity))
			{
				scene::EffectRecord value;
				value.name = effect->name;
				if (const auto* params = world.TryGet<EffectParamsComponent>(entity))
				{
					value.params = params->params;
				}
				record.effect = std::move(value);
			}
			if (const auto* image = world.TryGet<ui::UIImage>(entity))
			{
				record.uiImage = scene::UIImageRecord{.color = image->color, .cornerRadius = image->cornerRadius, .pixelArt = image->pixelArt, .texturePath = image->texturePath};
			}
			for (const auto& type: reflect::ComponentTypes())
			{
				if (!type.genericSerialize || !type.tryGetRawConst)
				{
					continue;
				}
				const void* component = type.tryGetRawConst(world, entity);
				if (!component)
				{
					continue;
				}
				scene::GenericComponent generic;
				generic.type = type.name;
				for (const auto& field: type.fields)
				{
					auto value = field.get(component);
					if (auto result = scene::CaptureStreamingFieldReference(world, entity, type.name, field.name, value, referenceIndex); !result)
						return std::unexpected(result.error());
					generic.fields.emplace_back(field.name, std::move(value));
				}
				record.reflected.push_back(std::move(generic));
			}
			if (const auto* collider = world.TryGet<ColliderComponent>(entity))
			{
				scene::PhysicsRecord physics;
				physics.shapeType = collider->shape;
				physics.halfExtents = collider->halfExtents;
				physics.radius = collider->radius;
				physics.halfHeight = collider->halfHeight;
				physics.center = collider->center;
				physics.friction = collider->friction;
				physics.restitution = collider->restitution;
				physics.isSensor = collider->isSensor;
				physics.meshSource = collider->meshSource;
				if (const auto* body = world.TryGet<RigidBodyComponent>(entity))
				{
					physics.motionType = body->motionType;
					physics.mass = body->mass;
					physics.linearDamping = body->linearDamping;
					physics.angularDamping = body->angularDamping;
					physics.gravityFactor = body->gravityFactor;
					physics.maxLinearVelocity = body->maxLinearVelocity;
					physics.maxAngularVelocity = body->maxAngularVelocity;
					physics.continuousCollision = body->continuousCollision;
					physics.allowSleeping = body->allowSleeping;
					physics.startActive = body->startActive;
					physics.lockPosition = body->lockPosition;
					physics.lockRotation = body->lockRotation;
				}
				else
				{
					physics.motionType = PhysicsMotionType::Static;
				}
				record.physics = std::move(physics);
			}
			if (const auto* joint = world.TryGet<JointComponent>(entity))
			{
				scene::JointRecord value;
				value.type = joint->type;
				value.targetIndex = targetIndex(joint->target);
				if (value.targetIndex == -2)
				{
					return std::unexpected(SnapshotError("Browser streaming joint target lies outside region"));
				}
				value.anchor = joint->anchor;
				value.axis = joint->axis;
				value.minLimit = joint->minLimit;
				value.maxLimit = joint->maxLimit;
				value.distance = joint->distance;
				value.collideConnected = joint->collideConnected;
				record.joint = value;
			}
			if (const auto* joints = world.TryGet<ScriptJointsComponent>(entity))
			{
				for (const auto& joint: joints->joints)
				{
					scene::ScriptJointRecord value;
					value.type = joint.type;
					value.targetIndex = targetIndex(joint.target);
					if (value.targetIndex == -2)
					{
						return std::unexpected(SnapshotError("Browser streaming script joint target lies outside region"));
					}
					value.anchor = joint.anchor;
					value.axis = joint.axis;
					value.minLimit = joint.minLimit;
					value.maxLimit = joint.maxLimit;
					value.distance = joint.distance;
					value.swingLimit = joint.swingLimit;
					value.collideConnected = joint.collideConnected;
					record.scriptJoints.push_back(value);
				}
			}
			if (const auto* joint = world.TryGet<Joint2DComponent>(entity))
			{
				record.joint2D = *joint;
				record.joint2DTargetIndex = targetIndex(joint->target);
				if (record.joint2DTargetIndex == -2)
				{
					return std::unexpected(SnapshotError("Browser streaming 2D joint target lies outside region"));
				}
				record.joint2D->target = {};
				record.joint2D->jointId = 0;
			}
			if (const auto* scripts = world.TryGet<ScriptComponent>(entity))
			{
				for (const auto& script: scripts->scripts)
				{
					if (script.path.empty())
					{
						return std::unexpected(SnapshotError("Browser streaming script has no type"));
					}
					scene::ScriptRecord value;
					value.type = script.path;
					value.properties = scene::detail::ScriptPropsToSceneRefs(script.properties, indices);
					value.properties.erase("Self");
					for (const auto& [name, property]: script.properties)
					{
						if (name == "Self" || (property.type != ScriptPropertyValue::Type::Entity && property.type != ScriptPropertyValue::Type::Component) || property.i64 == 0 || indices.contains(static_cast<std::uint32_t>(property.i64)))
						{
							continue;
						}
						const Entity target{static_cast<std::uint32_t>(property.i64)};
						if (!world.GetRegistry().valid(World::ToEntt(target)))
						{
							return std::unexpected(SnapshotError("Invalid browser streaming script reference"));
						}
						auto* node = world.TryGet<SceneNodeComponent>(target);
						if (!node)
						{
							node = &world.Emplace<SceneNodeComponent>(target, SceneNodeComponent{scene::GenerateSceneNodeId()});
						}
						if (!node->id)
						{
							node->id = scene::GenerateSceneNodeId();
						}
						value.nodeRefs.emplace(name, node->id);
					}
					record.scripts.push_back(std::move(value));
				}
			}
			document.entities.push_back(std::move(record));
		}
		nodes.clear();
		for (auto&& [handle, node]: world.View<SceneNodeComponent>().each())
		{
			if (node.id && !nodes.insert(node.id).second)
			{
				return std::unexpected(SnapshotError("Ambiguous browser streaming scene node"));
			}
		}
		return document;
	}
} // namespace aether::app::web
