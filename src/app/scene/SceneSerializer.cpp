#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <entt/entt.hpp>
#include <toml++/toml.hpp>

#include "assets/AssetManager.hpp"
#include "effects/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	namespace
	{
		constexpr int kSceneVersion = 1;

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
	} // namespace

	// ── Capture ─────────────────────────────────────────────────────────────────

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		SceneDescription scene;
		auto& reg = world.GetRegistry();

		std::vector<Entity> order;
		std::unordered_map<std::uint32_t, int> indexOf;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (!e.IsValid())
			{
				continue;
			}
			indexOf[e.id] = static_cast<int>(order.size());
			order.push_back(e);
		}

		scene.entities.reserve(order.size());
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
			scene.entities.push_back(std::move(rec));
		}
		return scene;
	}

	// ── TOML write ──────────────────────────────────────────────────────────────

	std::string WriteToml(const SceneDescription& scene)
	{
		toml::table root;
		toml::table header;
		header.insert("version", kSceneVersion);
		header.insert("name", scene.name);
		root.insert("scene", std::move(header));

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

	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		std::vector<Entity> created;
		created.reserve(scene.entities.size());
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			created.push_back(world.Create());
		}

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

			if (rec.effect && !rec.effect->name.empty() && deps.effectManager != nullptr && deps.effectParams != nullptr && deps.assets != nullptr)
			{
				if (!effects::ApplyEntityEffect(world, e, rec.effect->name, *deps.effectManager, deps.assets->GetPipelineCache(), *deps.effectParams, &rec.effect->params))
				{
					AE_WARN(LogCategory::App, "Scene load: unknown effect '{}'", rec.effect->name);
				}
			}
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
		}
		return created;
	}

	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps)
	{
		const auto scene = ReadSceneFile(sceneName);
		if (!scene)
		{
			return false;
		}

		// Replace-all: collect first (Destroy mutates storage), then destroy -
		// on_destroy hooks release physics bodies, material slots, effect slots.
		auto& reg = world.GetRegistry();
		std::vector<Entity> doomed;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (reg.valid(handle))
			{
				const Entity e = World::FromEntt(handle);
				if (e.IsValid())
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
		}

		ApplyScene(*scene, world, deps);
		AE_INFO(LogCategory::App, "Scene loaded: {} ({} entities)", sceneName, scene->entities.size());
		return true;
	}
} // namespace aether::app::scene
