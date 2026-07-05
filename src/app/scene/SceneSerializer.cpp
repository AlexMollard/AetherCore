#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <entt/entt.hpp>
#include <toml++/toml.hpp>

#include "assets/AssetManager.hpp"
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
#include "scripting/SceneContext.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app::scene
{
	namespace
	{

		// ── enum <-> string ────────────────────────────────────────────────────

		const char* ShapeName(PhysicsShapeType t)
		{
			switch (t)
			{
				case PhysicsShapeType::Sphere: return "sphere";
				case PhysicsShapeType::Capsule: return "capsule";
				default: return "box";
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
			return PhysicsShapeType::Box;
		}

		const char* MotionName(PhysicsMotionType t)
		{
			switch (t)
			{
				case PhysicsMotionType::Static: return "static";
				case PhysicsMotionType::Kinematic: return "kinematic";
				default: return "dynamic";
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

		// ── toml helpers ───────────────────────────────────────────────────────

		toml::array Vec3ToToml(const glm::vec3& v)
		{
			return toml::array{v.x, v.y, v.z};
		}

		toml::array Vec4ToToml(const glm::vec4& v)
		{
			return toml::array{v.x, v.y, v.z, v.w};
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

		// ── Script property (de)serialization ─────────────────────────────────────
		// Each property is an inline table with an explicit type tag so values
		// round-trip unambiguously (int vs float, enum vs int):
		//   WalkSpeed = { t = "float", v = 12.0 }
		//   Tint      = { t = "vec3",  v = [1.0, 0.5, 0.0] }
		const char* ScriptPropTypeTag(ScriptPropertyValue::Type type)
		{
			switch (type)
			{
				case ScriptPropertyValue::Type::Float: return "float";
				case ScriptPropertyValue::Type::Int: return "int";
				case ScriptPropertyValue::Type::Bool: return "bool";
				case ScriptPropertyValue::Type::Vector3: return "vec3";
				case ScriptPropertyValue::Type::String: return "string";
				case ScriptPropertyValue::Type::Enum: return "enum";
				case ScriptPropertyValue::Type::None:
				default: return "none";
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
						entry.insert("v", static_cast<std::int64_t>(value.i64));
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

	// ── Capture ─────────────────────────────────────────────────────────────────

	namespace
	{
		// One EntityRecord per entity in `order` (parent refs resolve through
		// `indexOf`; parents outside the map record -1). Shared by full-scene
		// capture and prefab (subtree) capture.
		void AppendEntityRecords(SceneDescription& scene, World& world, const std::vector<Entity>& order, const std::unordered_map<std::uint32_t, int>& indexOf, const MaterialRegistry& materials, const TextureRegistry& textures)
		{
			scene.entities.reserve(scene.entities.size() + order.size());
			for (const Entity e: order)
			{
				EntityRecord rec;
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
				if (const auto* shape = world.TryGet<PhysicsDebugShapeComponent>(e))
				{
					const auto* rb = world.TryGet<RigidBodyComponent>(e);
					rec.physics = PhysicsRecord{.shapeType = shape->shapeType, .halfExtents = shape->halfExtents, .radius = shape->radius, .halfHeight = shape->halfHeight, .motionType = rb ? rb->motionType : PhysicsMotionType::Static};
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
					clean.baseCaptured = false; // re-base from the restored transform
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
				if (const auto* pl = world.TryGet<PointLightComponent>(e))
				{
					rec.pointLight = *pl;
				}
				if (const auto* sl = world.TryGet<SpotLightComponent>(e))
				{
					rec.spotLight = *sl;
				}
				if (const auto* script = world.TryGet<ScriptComponent>(e); script != nullptr && !script->path.empty())
				{
					rec.script = script->path;
					rec.scriptProperties = script->properties;
				}
				scene.entities.push_back(std::move(rec));
			}
		}
	} // namespace

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer)
	{
		SceneDescription scene;
		auto& reg = world.GetRegistry();

		// Punctual lights are entities now (LightComponents.hpp) and serialize
		// per-entity below; only the environment rig is renderer-level state.
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

		// Transient entities (and their subtrees) are script-owned runtime state
		// - excluded so boot auto-generation and Play snapshots never duplicate
		// them when the script respawns its own actors.
		std::vector<Entity> order;
		std::unordered_map<std::uint32_t, int> indexOf;
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
			indexOf[e.id] = static_cast<int>(order.size());
			order.push_back(e);
		}

		AppendEntityRecords(scene, world, order, indexOf, materials, textures);
		return scene;
	}

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		// Each subtree in parent-before-child order. A root's own parent (if
		// any) is outside the index map, so its record naturally gets
		// parentIndex -1. Unlike scene capture there is NO transient
		// exclusion: subtree capture takes exactly what you point it at (the
		// scripted player prefab is the flagship case).
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

	// ── TOML write ──────────────────────────────────────────────────────────────

	std::string WriteToml(const SceneDescription& scene)
	{
		toml::table root;
		toml::table header;
		header.insert("version", kSceneFormatVersion);
		header.insert("name", scene.name);
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

		// Legacy [[lights]] only survives a parse -> write round trip of an old
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
				t.insert("physics", std::move(p));
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
				toml::table b;
				b.insert("amplitude", rec.bob->amplitude);
				b.insert("frequency", rec.bob->frequency);
				b.insert("phase", rec.bob->phase);
				t.insert("bob", std::move(b));
			}
			if (rec.spin)
			{
				toml::table s;
				s.insert("euler_deg_per_sec", Vec3ToToml(rec.spin->eulerDegPerSec));
				t.insert("spin", std::move(s));
			}
			if (rec.orbit)
			{
				toml::table o;
				o.insert("center", Vec3ToToml(rec.orbit->center));
				o.insert("radius", rec.orbit->radius);
				o.insert("speed_deg", rec.orbit->angularSpeedDeg);
				o.insert("angle_deg", rec.orbit->angleDeg);
				o.insert("yaw_offset_deg", rec.orbit->yawOffsetDeg);
				o.insert("height", rec.orbit->height);
				t.insert("orbit", std::move(o));
			}
			if (rec.materialPulse)
			{
				toml::table p;
				p.insert("emissive_a", Vec3ToToml(rec.materialPulse->emissiveA));
				p.insert("emissive_b", Vec3ToToml(rec.materialPulse->emissiveB));
				p.insert("frequency", rec.materialPulse->frequency);
				t.insert("material_pulse", std::move(p));
			}
			if (rec.pointLight)
			{
				toml::table l;
				l.insert("color", Vec3ToToml(rec.pointLight->color));
				l.insert("intensity", rec.pointLight->intensity);
				l.insert("radius", rec.pointLight->radius);
				l.insert("shadow", rec.pointLight->castsShadow);
				t.insert("point_light", std::move(l));
			}
			if (rec.spotLight)
			{
				toml::table l;
				l.insert("color", Vec3ToToml(rec.spotLight->color));
				l.insert("intensity", rec.spotLight->intensity);
				l.insert("radius", rec.spotLight->radius);
				l.insert("inner_rad", rec.spotLight->innerAngleRad);
				l.insert("outer_rad", rec.spotLight->outerAngleRad);
				l.insert("shadow", rec.spotLight->castsShadow);
				t.insert("spot_light", std::move(l));
			}
			if (rec.script)
			{
				t.insert("script", *rec.script);
			}
			if (!rec.scriptProperties.empty())
			{
				t.insert("script_properties", ScriptPropsToToml(rec.scriptProperties));
			}
			entities.push_back(std::move(t));
		}
		root.insert("entities", std::move(entities));

		std::ostringstream out;
		out << "# AetherCore scene - generated by the debug editor\n" << root << "\n";
		return out.str();
	}

	// ── TOML parse ──────────────────────────────────────────────────────────────

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

		SceneDescription scene;
		scene.name = root["scene"]["name"].value_or(std::string{});
		scene.version = static_cast<int>(root["scene"]["version"].value_or(std::int64_t{1}));
		if (scene.version < kSceneFormatVersion)
		{
			AE_WARN(LogCategory::App, "Scene file '{}' is format v{} (current v{}): records added since it was written are absent (v2 added behaviors + lights/environment; v3 made lights entities - legacy [[lights]] migrate on load). Re-save from the editor to upgrade.", scene.name, scene.version, kSceneFormatVersion);
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

		const auto* entities = root["entities"].as_array();
		if (entities == nullptr)
		{
			return scene; // empty scene is legal
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
				mat.albedoPath = mv["albedo"].value_or(std::string{});
				mat.normalPath = mv["normal"].value_or(std::string{});
				mat.metallicRoughnessPath = mv["metallic_roughness"].value_or(std::string{});
				mat.occlusionPath = mv["occlusion_tex"].value_or(std::string{});
				mat.emissivePath = mv["emissive_tex"].value_or(std::string{});
				rec.material = std::move(mat);
			}
			if (const auto* s = tv["skinned"].as_table())
			{
				const toml::node_view<const toml::node> sv{*s};
				rec.skinned = SkinnedRecord{.clipIndex = static_cast<std::uint32_t>(sv["clip"].value_or(std::int64_t{0})), .animTime = static_cast<float>(sv["time"].value_or(0.0)), .playbackSpeed = static_cast<float>(sv["speed"].value_or(1.0)), .looping = sv["looping"].value_or(true)};
			}
			if (const auto* p = tv["physics"].as_table())
			{
				const toml::node_view<const toml::node> pv{*p};
				rec.physics = PhysicsRecord{.shapeType = ShapeFromName(pv["shape"].value_or(std::string{"box"})), .halfExtents = Vec3FromToml(pv["half_extents"], glm::vec3(0.5f)), .radius = static_cast<float>(pv["radius"].value_or(0.5)), .halfHeight = static_cast<float>(pv["half_height"].value_or(0.5)), .motionType = MotionFromName(pv["motion"].value_or(std::string{"dynamic"}))};
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
				const toml::node_view<const toml::node> bv{*b};
				rec.bob = BobComponent{.amplitude = static_cast<float>(bv["amplitude"].value_or(1.0)), .frequency = static_cast<float>(bv["frequency"].value_or(1.0)), .phase = static_cast<float>(bv["phase"].value_or(0.0))};
			}
			if (const auto* s = tv["spin"].as_table())
			{
				const toml::node_view<const toml::node> sv{*s};
				rec.spin = SpinComponent{.eulerDegPerSec = Vec3FromToml(sv["euler_deg_per_sec"], glm::vec3(0.0f, 40.0f, 0.0f))};
			}
			if (const auto* o = tv["orbit"].as_table())
			{
				const toml::node_view<const toml::node> ov{*o};
				rec.orbit = OrbitComponent{.center = Vec3FromToml(ov["center"], glm::vec3(0.0f)), .radius = static_cast<float>(ov["radius"].value_or(5.0)), .angularSpeedDeg = static_cast<float>(ov["speed_deg"].value_or(30.0)), .angleDeg = static_cast<float>(ov["angle_deg"].value_or(0.0)), .yawOffsetDeg = static_cast<float>(ov["yaw_offset_deg"].value_or(0.0)), .height = static_cast<float>(ov["height"].value_or(0.0))};
			}
			if (const auto* p = tv["material_pulse"].as_table())
			{
				const toml::node_view<const toml::node> pv{*p};
				rec.materialPulse = MaterialPulseComponent{.emissiveA = Vec3FromToml(pv["emissive_a"], glm::vec3(0.0f)), .emissiveB = Vec3FromToml(pv["emissive_b"], glm::vec3(1.0f, 0.5f, 0.1f)), .frequency = static_cast<float>(pv["frequency"].value_or(2.0))};
			}
			if (const auto* l = tv["point_light"].as_table())
			{
				const toml::node_view<const toml::node> lv{*l};
				rec.pointLight = PointLightComponent{.color = Vec3FromToml(lv["color"], glm::vec3(1.0f)), .intensity = static_cast<float>(lv["intensity"].value_or(20.0)), .radius = static_cast<float>(lv["radius"].value_or(15.0)), .castsShadow = lv["shadow"].value_or(false)};
			}
			if (const auto* l = tv["spot_light"].as_table())
			{
				const toml::node_view<const toml::node> lv{*l};
				rec.spotLight = SpotLightComponent{.color = Vec3FromToml(lv["color"], glm::vec3(1.0f)), .intensity = static_cast<float>(lv["intensity"].value_or(30.0)), .radius = static_cast<float>(lv["radius"].value_or(30.0)), .innerAngleRad = static_cast<float>(lv["inner_rad"].value_or(0.35)), .outerAngleRad = static_cast<float>(lv["outer_rad"].value_or(0.60)), .castsShadow = lv["shadow"].value_or(false)};
			}
			if (const auto script = tv["script"].value<std::string>(); script.has_value() && !script->empty())
			{
				rec.script = *script;
			}
			if (const auto* props = tv["script_properties"].as_table())
			{
				rec.scriptProperties = ScriptPropsFromToml(*props);
			}
			scene.entities.push_back(std::move(rec));
		}
		return scene;
	}

	// ── Files ───────────────────────────────────────────────────────────────────

	std::string ScenesDirectory()
	{
#ifdef AETHER_SCENES_SOURCE_DIR
		return AETHER_SCENES_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("scenes").string();
#endif
	}

	std::string PrefabsDirectory()
	{
#ifdef AETHER_PREFABS_SOURCE_DIR
		return AETHER_PREFABS_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("prefabs").string();
#endif
	}

	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefab)
	{
		const std::filesystem::path dir{PrefabsDirectory()};
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path path = dir / (prefabName + ".prefab.toml");

		std::ofstream out(path, std::ios::trunc);
		if (!out.is_open())
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot open '{}'", path.string());
			return false;
		}
		out << WriteToml(prefab);
		AE_INFO(LogCategory::App, "Prefab saved: {} ({} entities)", path.string(), prefab.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName)
	{
		const std::filesystem::path path = std::filesystem::path{PrefabsDirectory()} / (prefabName + ".prefab.toml");
		std::ifstream in(path);
		if (!in.is_open())
		{
			AE_WARN(LogCategory::App, "ReadPrefabFile: cannot open '{}'", path.string());
			return std::nullopt;
		}
		std::stringstream buffer;
		buffer << in.rdbuf();
		return ParseToml(buffer.str());
	}

	std::vector<std::string> ListPrefabFiles()
	{
		std::vector<std::string> names;
		const std::filesystem::path dir{PrefabsDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = ".prefab.toml";
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
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path path = dir / (sceneName + ".scene.toml");

		std::ofstream out(path, std::ios::trunc);
		if (!out.is_open())
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot open '{}'", path.string());
			return false;
		}
		out << WriteToml(scene);
		AE_INFO(LogCategory::App, "Scene saved: {} ({} entities)", path.string(), scene.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName)
	{
		const std::filesystem::path path = std::filesystem::path{ScenesDirectory()} / (sceneName + ".scene.toml");
		std::ifstream in(path);
		if (!in.is_open())
		{
			AE_WARN(LogCategory::App, "ReadSceneFile: cannot open '{}'", path.string());
			return std::nullopt;
		}
		std::stringstream buffer;
		buffer << in.rdbuf();
		return ParseToml(buffer.str());
	}

	std::vector<std::string> ListSceneFiles()
	{
		std::vector<std::string> names;
		const std::filesystem::path dir{ScenesDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = ".scene.toml";
			if (file.size() > kSuffix.size() && file.ends_with(kSuffix))
			{
				names.push_back(file.substr(0, file.size() - kSuffix.size()));
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	// ── Apply / load ────────────────────────────────────────────────────────────

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
		return deps;
	}

	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		// Environment rig (sun/ambient/sky) is renderer-level state; punctual
		// lights are entities and arrive with the records below (or migrate
		// from the legacy [[lights]] list at the end).
		if (deps.renderer != nullptr && scene.environment)
		{
			const EnvironmentRecord& env = *scene.environment;
			deps.renderer->SetAmbientLight(env.ambient);
			deps.renderer->SetDirectionalLight(env.sunDirection, env.sunIntensity);
			deps.renderer->SetSunColor(env.sunColor);
			deps.renderer->SetSkyGradient(env.skyHorizon, env.skyZenith);
			deps.renderer->SetSkyVoidColor(env.skyVoid);
		}

		std::vector<Entity> created;
		created.reserve(scene.entities.size());
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			created.push_back(world.Create());
		}

		// Apply-health counters: surfaced in the summary log below so a load
		// that silently degrades (missing deps, unknown effects, old file
		// format) is visible in the log instead of just "looking wrong".
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
			if (rec.hasTransform)
			{
				world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(rec.position, rec.eulerDeg, rec.scale)});
			}

			// Physics: emplace the same descriptor the das bindings emplace (only
			// shape + motion are authored); FlushPendingBodies builds the body.
			if (rec.physics)
			{
				switch (rec.physics->shapeType)
				{
					case PhysicsShapeType::Sphere:
					{
						SphereBodyDesc desc{};
						desc.radius = rec.physics->radius;
						desc.motionType = rec.physics->motionType;
						world.Emplace<SphereBodyDesc>(e, desc);
						break;
					}
					case PhysicsShapeType::Capsule:
					{
						CapsuleBodyDesc desc{};
						desc.halfHeight = rec.physics->halfHeight;
						desc.radius = rec.physics->radius;
						desc.motionType = rec.physics->motionType;
						world.Emplace<CapsuleBodyDesc>(e, desc);
						break;
					}
					default:
					{
						BoxBodyDesc desc{};
						desc.halfExtents = rec.physics->halfExtents;
						desc.motionType = rec.physics->motionType;
						world.Emplace<BoxBodyDesc>(e, desc);
						break;
					}
				}
			}

			// Mesh (and, for model primitives, the skinned setup that needs the
			// model's AnimationDatabase).
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

			// Material before effect: AssignMaterial resolves the material
			// pipeline, then an effect (if any) overrides it - final state matches
			// the original authoring order.
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
				MaterialSystem::AssignMaterial(world, e, deps.assets->GetMaterialRegistry(), deps.assets->GetPipelineCache(), asset);
				// Registry Acquire (inside AssignMaterial's cascade) now owns the
				// texture refs; drop the ones this scope took.
				for (TextureHandle h: {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex})
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
			if (rec.pointLight)
			{
				world.Emplace<PointLightComponent>(e, *rec.pointLight);
			}
			if (rec.spotLight)
			{
				world.Emplace<SpotLightComponent>(e, *rec.spotLight);
			}
			if (rec.script)
			{
				// attached stays false: the script system re-attaches on the
				// next play tick (loads and Stop-restores restart scripts).
				world.Emplace<ScriptComponent>(e, ScriptComponent{.path = *rec.script, .properties = rec.scriptProperties});
			}
		}

		// Legacy [[lights]] (pre-v3 files): promote each record to a light
		// entity so it shows in the outliner and re-saves in the new format.
		std::vector<Entity> migratedLights;
		for (const LightRecord& light: scene.lights)
		{
			if (light.isSpot)
			{
				migratedLights.push_back(ecs::CreateSpotLightEntity(world, light.position, light.direction, SpotLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .innerAngleRad = light.innerAngleRad, .outerAngleRad = light.outerAngleRad, .castsShadow = light.castsShadow}));
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

		// Hierarchy after every entity exists.
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			const int parent = scene.entities[i].parentIndex;
			if (parent >= 0 && static_cast<std::size_t>(parent) < created.size())
			{
				ecs::SetParent(world, created[i], created[static_cast<std::size_t>(parent)]);
			}
		}

		if (deps.sceneContext != nullptr)
		{
			for (const Entity e: created)
			{
				deps.sceneContext->sceneEntities.push_back(e);
			}
			// Migrated legacy lights are scene content too - F5 teardown owns them.
			for (const Entity e: migratedLights)
			{
				deps.sceneContext->sceneEntities.push_back(e);
			}
		}
		AE_INFO(LogCategory::App, "Scene apply: {} entities, {} behaviors, {} effects (format v{})", created.size() + migratedLights.size(), behaviorCount, effectCount, scene.version);
		return created;
	}

	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		// Collect first (Destroy mutates storage), then destroy - on_destroy
		// hooks release physics bodies, material slots, effect slots. Body
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
				// Transient subtrees are the mirror image of capture's
				// exclusion: the snapshot deliberately left them out (script-
				// owned actors like the player), so replace-all must leave
				// them ALIVE - destroying them here left no player until the
				// script next respawned it, and their das-held ids went stale.
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
			// Spared entities stay registered so F5's teardown still owns them.
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

		// Re-root: the capture order guarantees exactly one parentless record
		// (the subtree root, always first, but search to stay robust to
		// hand-edited files). The subtree keeps its internal offsets via the
		// delta-propagating transform write.
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
