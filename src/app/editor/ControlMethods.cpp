#include "editor/ControlMethods.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "PlaySession.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "debug/EditorWindowActions.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/ComponentFields.hpp"
#include "editor/ModelImport.hpp"
#include "editor/ReflectionJson.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/PlatformPaths.hpp"
#include "rendering/ScreenshotService.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/SettingsService.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scripting/SceneContext.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/RenderGraphStorage.hpp" // FrameStats definition (GetFrameStats)

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		// ── JSON-Schema helpers (kept terse so method entries read cleanly) ──────
		const json kVec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};

		json Obj(json properties = json::object(), std::vector<std::string> required = {})
		{
			json schema{{"type", "object"}, {"properties", std::move(properties)}};
			if (!required.empty())
			{
				schema["required"] = required;
			}
			return schema;
		}

		json IntProp()
		{
			return json{{"type", "integer"}};
		}

		json StrProp()
		{
			return json{{"type", "string"}};
		}

		// Registry debug names are "<logical> (file:line)"; the logical prefix is the
		// stable, unique id the endpoint exposes (callers never pass the source site).
		std::string LogicalTexName(const std::string& debugName)
		{
			const std::size_t paren = debugName.rfind(" (");
			return paren == std::string::npos ? debugName : debugName.substr(0, paren);
		}

		// ── value helpers ────────────────────────────────────────────────────────
		std::uint32_t IdOf(const json& p, const char* key = "id")
		{
			return static_cast<std::uint32_t>(p.value(key, static_cast<std::uint32_t>(0)));
		}

		glm::vec3 ReadVec3(const json& obj, const char* key, glm::vec3 fallback)
		{
			if (!obj.contains(key) || !obj[key].is_array() || obj[key].size() != 3)
			{
				return fallback;
			}
			const auto& a = obj[key];
			return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
		}

		json Vec3ToJson(const glm::vec3& v)
		{
			return json::array({v.x, v.y, v.z});
		}

		json ErrNoScene()
		{
			return json{{"error", "no scene subsystem"}};
		}

		json ErrNoEntity()
		{
			return json{{"error", "no such entity"}};
		}
	} // namespace

	std::vector<ControlMethod> BuildControlMethods()
	{
		std::vector<ControlMethod> methods;

		methods.push_back({"info",
		        "engine_info",
		        "Live editor summary: current scene name, entity count, frame index, fps.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        std::size_t count = 0;
			        if (scenes != nullptr)
			        {
				        for ([[maybe_unused]] auto e: scenes->GetWorld().View<NameComponent>())
				        {
					        ++count;
				        }
			        }
			        const auto* playState = ctx.services.TryGet<app::PlayState>();
			        const char* mode = "editing";
			        if (playState != nullptr)
			        {
				        mode = playState->IsPlaying() ? "playing" : (playState->IsCompiling() ? "compiling" : "editing");
			        }
			        return json{{"frame", ctx.frameIndex}, {"fps", ctx.fps}, {"scene", scenes != nullptr ? scenes->GetCurrentScene() : ""}, {"entities", count}, {"playState", mode}};
		        }});

		methods.push_back({"scene.entities",
		        "list_entities",
		        "List every entity in the live scene with id, name, and world position.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        json arr = json::array();
			        if (scenes != nullptr)
			        {
				        World& world = scenes->GetWorld();
				        for (auto enttEntity: world.View<NameComponent>())
				        {
					        const Entity entity = World::FromEntt(enttEntity);
					        json e{{"id", entity.id}, {"name", world.Get<NameComponent>(entity).name}};
					        if (const auto* t = world.TryGet<TransformComponent>(entity))
					        {
						        e["position"] = Vec3ToJson(glm::vec3(t->localToWorld[3]));
					        }
					        arr.push_back(std::move(e));
				        }
			        }
			        return json{{"entities", arr}};
		        }});

		methods.push_back({"scene.get",
		        "get_entity",
		        "Full detail for one entity by id: name, world position, scale, and its component type list.",
		        false,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        const entt::entity enttEntity = World::ToEntt(entity);
			        if (!world.GetRegistry().valid(enttEntity))
			        {
				        return ErrNoEntity();
			        }
			        json j{{"id", entity.id}};
			        if (const auto* n = world.TryGet<NameComponent>(entity))
			        {
				        j["name"] = n->name;
			        }
			        if (const auto* t = world.TryGet<TransformComponent>(entity))
			        {
				        const glm::mat4& m = t->localToWorld;
				        j["position"] = Vec3ToJson(glm::vec3(m[3]));
				        j["scale"] = Vec3ToJson(glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))));
			        }
			        json comps = json::array();
			        for (auto&& [typeId, storage]: world.GetRegistry().storage())
			        {
				        if (storage.contains(enttEntity))
				        {
					        comps.push_back(std::string(storage.type().name()));
				        }
			        }
			        j["components"] = comps;
			        return j;
		        }});

		methods.push_back({"scene.create",
		        "create_entity",
		        "Create an entity in the live scene. Returns its id.",
		        true,
		        Obj({{"name", StrProp()}, {"position", kVec3}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity = world.Create();
			        const std::string name = p.value("name", std::string("Entity"));
			        world.Emplace<NameComponent>(entity, name);
			        world.Emplace<TransformComponent>(entity, TransformComponent{glm::translate(glm::mat4(1.0f), ReadVec3(p, "position", glm::vec3(0.0f)))});
			        world.RegisterRoot(entity);
			        return json{{"id", entity.id}, {"name", name}};
		        }});

		methods.push_back({"scene.add_model",
		        "add_model",
		        "Add a model file to the live scene as one operation - the seamless equivalent of dragging a glTF into the editor. Bakes the raw glTF if it hasn't been imported yet, then spawns the correct layout: a lone static mesh sits on a "
		        "single entity, while a multi-primitive or skinned model becomes a root with one child per primitive (each carrying its own material and skin). 'path' is a project:// or absolute model path; optional 'name', 'position', "
		        "'rotationEuler' (degrees) and 'scale' place it (skinned models authored in centimetres usually want scale ~0.01). Returns the root id and primitive count.",
		        true,
		        Obj({{"path", StrProp()}, {"name", StrProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"path"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        const std::string path = p.value("path", std::string{});
			        if (path.empty())
			        {
				        return json{{"error", "'path' is required"}};
			        }
			        World& world = scenes->GetWorld();

			        const glm::vec3 pos = ReadVec3(p, "position", glm::vec3(0.0f));
			        const glm::vec3 euler = ReadVec3(p, "rotationEuler", glm::vec3(0.0f));
			        const glm::vec3 scl = ReadVec3(p, "scale", glm::vec3(1.0f));
			        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
			        m = glm::rotate(m, glm::radians(euler.z), glm::vec3(0, 0, 1));
			        m = glm::rotate(m, glm::radians(euler.y), glm::vec3(0, 1, 0));
			        m = glm::rotate(m, glm::radians(euler.x), glm::vec3(1, 0, 0));
			        m = glm::scale(m, scl);

			        std::string error;
			        const Entity root = editor::ImportModelIntoScene(world, ctx.services, path, m, p.value("name", std::string{}), error);
			        if (!root.IsValid())
			        {
				        return json{{"error", error.empty() ? std::string("model import failed") : error}};
			        }

			        std::string name = path;
			        if (const auto* nc = world.TryGet<NameComponent>(root))
			        {
				        name = nc->name;
			        }
			        int prims = 1;
			        if (auto* assets = ctx.services.TryGet<AssetManager>())
			        {
				        if (auto* sc = ctx.services.TryGet<app::scripting::SceneContext>())
				        {
					        prims = app::scene::ModelPrimitiveCount(*assets, *sc, path);
				        }
			        }
			        return json{{"id", root.id}, {"name", name}, {"primitives", prims}};
		        }});

		methods.push_back({"scene.rename",
		        "rename_entity",
		        "Rename an entity by id.",
		        true,
		        Obj({{"id", IntProp()}, {"name", StrProp()}}, {"id", "name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        const std::string name = p.value("name", std::string{});
			        if (name.empty())
			        {
				        return json{{"error", "'name' is required"}};
			        }
			        world.EmplaceOrReplace<NameComponent>(entity, NameComponent{.name = name});
			        return json{{"id", entity.id}, {"name", name}};
		        }});

		methods.push_back({"scene.parent",
		        "parent_entity",
		        "Set an entity's parent by id. parent=0 unparents it to the scene root.",
		        true,
		        Obj({{"id", IntProp()}, {"parent", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity child{IdOf(p)};
			        const Entity parent{IdOf(p, "parent")};
			        if (!world.GetRegistry().valid(World::ToEntt(child)))
			        {
				        return ErrNoEntity();
			        }
			        if (parent.id != 0 && !world.GetRegistry().valid(World::ToEntt(parent)))
			        {
				        return json{{"error", "no such parent"}};
			        }
			        const bool ok = aether::ecs::SetParent(world, child, parent);
			        return json{{"id", child.id}, {"parent", parent.id}, {"ok", ok}};
		        }});

		methods.push_back({"scene.transform",
		        "set_transform",
		        "Set an entity's transform (position / rotationEuler in degrees / scale) by id.",
		        true,
		        Obj({{"id", IntProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        const glm::vec3 pos = ReadVec3(p, "position", glm::vec3(0.0f));
			        const glm::vec3 euler = ReadVec3(p, "rotationEuler", glm::vec3(0.0f));
			        const glm::vec3 scale = ReadVec3(p, "scale", glm::vec3(1.0f));
			        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
			        m = glm::rotate(m, glm::radians(euler.z), glm::vec3(0, 0, 1));
			        m = glm::rotate(m, glm::radians(euler.y), glm::vec3(0, 1, 0));
			        m = glm::rotate(m, glm::radians(euler.x), glm::vec3(1, 0, 0));
			        m = glm::scale(m, scale);
			        world.EmplaceOrReplace<TransformComponent>(entity, TransformComponent{m});
			        return json{{"id", entity.id}, {"ok", true}};
		        }});

		methods.push_back({"scene.delete",
		        "delete_entity",
		        "Delete an entity from the live scene by id.",
		        true,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        world.Destroy(entity);
			        return json{{"id", entity.id}, {"deleted", true}};
		        }});

		// Component add/remove drives the shared ComponentCatalog - the same source
		// the Inspector's Add-Component palette uses, so the two never drift.
		const auto componentOp = [](bool add)
		{
			return [add](const json& p, MethodContext& ctx) -> json
			{
				auto* scenes = ctx.services.TryGet<SceneSubsystem>();
				if (scenes == nullptr)
				{
					return ErrNoScene();
				}
				World& world = scenes->GetWorld();
				const Entity entity{IdOf(p)};
				if (!world.GetRegistry().valid(World::ToEntt(entity)))
				{
					return ErrNoEntity();
				}
				const std::string type = p.value("type", std::string{});
				const ComponentCatalogEntry* entry = FindComponent(type);
				if (entry == nullptr)
				{
					return json{{"error", "unknown component '" + type + "' (call list_component_types)"}};
				}
				if (!entry->addable || (add ? !entry->add : !entry->remove))
				{
					return json{{"error", "'" + type + "' is reference-only and cannot be added/removed as a component (e.g. UI Text is authored as a UI entity)"}};
				}
				if (add)
				{
					entry->add(world, entity, ctx.services);
				}
				else
				{
					entry->remove(world, entity);
				}
				return json{{"id", entity.id}, {"type", type}, {add ? "added" : "removed", true}};
			};
		};
		methods.push_back({"scene.add_component",
		        "add_component",
		        "Add a component to an entity by id. 'type' is a ComponentCatalog name (call list_component_types for the set).",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}),
		        componentOp(true)});
		methods.push_back({"scene.remove_component", "remove_component", "Remove a component from an entity by id ('type' is a ComponentCatalog name).", true, Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}), componentOp(false)});

		methods.push_back({"scene.get_component",
		        "get_component",
		        "Read a component's editable fields - the same fields the Inspector shows - as a JSON object. 'type' is a component name (call list_component_types for the set and each type's fields). Returns {id, type, fields:{...}}, or an error "
		        "if the entity lacks that component.",
		        false,
		        Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        const std::string type = p.value("type", std::string{});
			        // Reflection-driven (covers every migrated component); falls back to the
			        // legacy ComponentFields registry for not-yet-migrated types.
			        if (const auto* rt = reflect::FindComponentType(type))
			        {
				        const void* comp = rt->tryGetRawConst(world, entity);
				        if (comp == nullptr)
				        {
					        return json{{"error", "entity has no '" + type + "' component"}};
				        }
				        json out = json::object();
				        for (const auto& f: rt->fields)
				        {
					        const reflect::FieldValue fv = f.get(comp);
					        // Enums read out as their name string (set accepts name or int).
					        if (f.type == reflect::FieldType::Enum && f.meta.enumTable != nullptr)
					        {
						        out[f.name] = f.meta.enumTable->NameOf(fv.enumValue);
					        }
					        else
					        {
						        out[f.name] = editor::FieldValueToJson(fv);
					        }
				        }
				        return json{{"id", entity.id}, {"type", type}, {"fields", out}};
			        }
			        const auto* fields = editor::FindComponentFields(type);
			        if (fields == nullptr)
			        {
				        return json{{"error", "component '" + type + "' has no editable fields"}};
			        }
			        json out = json::object();
			        if (!fields->read(world, entity, ctx.services, out))
			        {
				        return json{{"error", "entity has no '" + type + "' component"}};
			        }
			        return json{{"id", entity.id}, {"type", type}, {"fields", out}};
		        }});

		methods.push_back({"scene.set_component",
		        "set_component",
		        "Set one or more editable fields on a component - the programmatic equivalent of editing it in the Inspector. 'type' is a component name; 'values' is an object mapping field name -> value for ONLY the fields you want to change "
		        "(partial update; call get_component or list_component_types for field names/types). Colours/vectors are [x,y,z(,w)] arrays. Returns {id, type, applied:[...]}.",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}, {"values", json{{"type", "object"}}}}, {"id", "type", "values"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        const std::string type = p.value("type", std::string{});
			        const json values = (p.contains("values") && p["values"].is_object()) ? p["values"] : json::object();
			        if (const auto* rt = reflect::FindComponentType(type))
			        {
				        void* comp = rt->tryGetRaw(world, entity);
				        if (comp == nullptr)
				        {
					        return json{{"error", "entity has no '" + type + "' component"}};
				        }
				        json applied = json::array();
				        for (const auto& f: rt->fields)
				        {
					        if (values.contains(f.name))
					        {
						        f.set(comp, editor::JsonToFieldValue(values.at(f.name), f));
						        applied.push_back(f.name);
					        }
				        }
				        if (applied.empty())
				        {
					        return json{{"error", "'values' named no known fields of '" + type + "'"}};
				        }
				        if (rt->postSet)
				        {
					        rt->postSet(world, entity);
				        }
				        return json{{"id", entity.id}, {"type", type}, {"applied", applied}};
			        }
			        const auto* fields = editor::FindComponentFields(type);
			        if (fields == nullptr)
			        {
				        return json{{"error", "component '" + type + "' has no editable fields"}};
			        }
			        const auto applied = fields->write(world, entity, values, ctx.services);
			        if (applied.empty())
			        {
				        return json{{"error", "entity has no '" + type + "' component, or 'values' named no known fields"}};
			        }
			        return json{{"id", entity.id}, {"type", type}, {"applied", applied}};
		        }});

		methods.push_back({"scene.component_types",
		        "list_component_types",
		        "List every component (name + category), plus, for components with editable fields, the field list get_component / set_component accept.",
		        false,
		        Obj(),
		        [](const json&, MethodContext&) -> json
		        {
			        // Build a "name:type, ..." hint from a reflected component's fields.
			        const auto reflectedHint = [](const reflect::ComponentType& rt) -> std::string
			        {
				        std::string h;
				        for (const auto& f: rt.fields)
				        {
					        h += (h.empty() ? "" : ", ") + f.name + ":" + editor::FieldTypeName(f.type);
				        }
				        return h;
			        };
			        json arr = json::array();
			        std::vector<std::string> listed;
			        for (const ComponentCatalogEntry& e: ComponentCatalog())
			        {
				        if (!e.addable)
				        {
					        continue;
				        } // reference-only entries can't be added
				        json entry = {{"name", e.name}, {"category", e.category}};
				        if (const auto* rt = reflect::FindComponentType(e.name))
				        {
					        entry["fields"] = reflectedHint(*rt);
				        }
				        else if (const auto* fields = editor::FindComponentFields(e.name))
				        {
					        entry["fields"] = fields->fields;
				        }
				        arr.push_back(entry);
				        listed.push_back(e.name);
			        }
			        // Reflected components not in the add-palette (e.g. Transform, Skinned Mesh)
			        // are still get/set-able - list them so agents can discover their fields.
			        for (const reflect::ComponentType& rt: reflect::ComponentTypes())
			        {
				        if (std::find(listed.begin(), listed.end(), rt.name) != listed.end())
				        {
					        continue;
				        }
				        arr.push_back(json{{"name", rt.name}, {"category", rt.category}, {"fields", reflectedHint(rt)}});
			        }
			        return json{{"components", arr}};
		        }});

		methods.push_back({"scene.save",
		        "save_scene",
		        "Save the live scene to its committable .scene.toml. Defaults to the current scene; pass 'name' to save under a different name.",
		        true,
		        Obj({{"name", StrProp()}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        auto* assets = ctx.services.TryGet<AssetManager>();
			        if (scenes == nullptr || assets == nullptr)
			        {
				        return json{{"error", "no scene/asset subsystem"}};
			        }
			        const std::string name = p.value("name", scenes->GetCurrentScene());
			        if (name.empty())
			        {
				        return json{{"error", "no scene name (open a scene first, or pass 'name')"}};
			        }
			        const bool ok = app::scene::QuickSave(ctx.services.Get<World>(), name, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());
			        return json{{"saved", ok}, {"scene", name}};
		        }});

		methods.push_back({"scene.load",
		        "load_scene",
		        "Switch the live editor to a scene by name (tears the current scene down first).",
		        true,
		        Obj({{"name", StrProp()}}, {"name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        const std::string name = p.value("name", std::string{});
			        if (name.empty())
			        {
				        return json{{"error", "'name' is required"}};
			        }
			        const bool loaded = app::scene::SwitchScene(name, scenes->GetWorld(), app::scene::MakeApplySceneDeps(ctx.services));
			        if (loaded)
			        {
				        scenes->SetCurrentScene(name);
			        }
			        return json{{"scene", name}, {"loaded", loaded}};
		        }});

		methods.push_back({"scene.new",
		        "new_scene",
		        "Replace the live scene with a fresh empty scene.",
		        true,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        const std::string name = app::scene::NewScene(scenes->GetWorld(), app::scene::MakeApplySceneDeps(ctx.services));
			        if (!name.empty())
			        {
				        scenes->SetCurrentScene(name);
			        }
			        return json{{"scene", name}, {"ok", !name.empty()}};
		        }});

		// Play/stop share the PlaySession free functions; each is its own entry so
		// each appears as a distinct MCP tool.
		const auto playHandler = [](const char* which)
		{
			return [which](const json&, MethodContext& ctx) -> json
			{
				app::LayerContext lc{.services = ctx.services, .frameIndex = ctx.frameIndex};
				bool ok = false;
				const std::string w = which;
				if (w == "play")
				{
					ok = StartPlaySession(lc);
				}
				else if (w == "stop")
				{
					ok = StopPlaySession(lc);
				}
				else
				{
					ok = TogglePlaySession(lc);
				}
				const auto* playState = ctx.services.TryGet<app::PlayState>();
				const char* state = "editing";
				if (playState != nullptr)
				{
					state = playState->IsPlaying() ? "playing" : (playState->IsCompiling() ? "compiling" : "editing");
				}
				// Play is async: entering it returns state="compiling" immediately while
				// the C# build runs on a worker thread. Poll `info`.playState until it
				// reads "playing".
				return json{{"ok", ok}, {"playing", playState != nullptr && playState->IsPlaying()}, {"state", state}};
			};
		};
		methods.push_back({"engine.play",
		        "play",
		        "Enter Play mode (async): rebuild the C# scripts on a worker thread, then snapshot + start. Returns immediately with state='compiling'; poll info.playState until 'playing'. The editor never blocks. Mirrors the Play button.",
		        true,
		        Obj(),
		        playHandler("play")});
		methods.push_back({"engine.stop", "stop", "Exit Play mode and restore the pre-play scene snapshot.", true, Obj(), playHandler("stop")});
		methods.push_back({"engine.toggle_play", "toggle_play", "Toggle Play/Stop.", true, Obj(), playHandler("toggle")});

		methods.push_back({"rendergraph",
		        "query_rendergraph",
		        "Dump the live render graph: every compiled pass with its type, dependencies, produced/consumed frame products, and last CPU time.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* rendering = ctx.services.TryGet<RenderingSubsystem>();
			        if (rendering == nullptr)
			        {
				        return json{{"error", "no rendering subsystem"}};
			        }
			        json arr = json::array();
			        for (const RenderGraph::PassInfo& pass: rendering->GetRenderGraph().GetPasses())
			        {
				        arr.push_back(json{{"name", pass.name},
				                {"index", pass.index},
				                {"graphics", pass.isGraphics},
				                {"compute", pass.isCompute},
				                {"asyncCompute", pass.isAsyncCompute},
				                {"compiled", pass.isCompiled},
				                {"culled", pass.isCulled},
				                {"dependencies", pass.logicalDependencies},
				                {"producedFrameProducts", pass.producedFrameProducts},
				                {"consumedFrameProducts", pass.consumedFrameProducts},
				                {"colorWriteCount", pass.colorWriteCount},
				                {"hasDepthWrite", pass.hasDepthWrite},
				                {"cpuTimeMs", pass.lastCpuTimeMs}});
			        }
			        return json{{"passes", arr}};
		        }});

		methods.push_back({"render.stats",
		        "render_stats",
		        "Render-graph frame statistics: pass/barrier counts, transient-image cache hit/miss, and the transient GPU heap capacity vs. bytes used - the engine's per-frame allocation profile.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* rendering = ctx.services.TryGet<RenderingSubsystem>();
			        if (rendering == nullptr)
			        {
				        return json{{"error", "no rendering subsystem"}};
			        }
			        const FrameStats& s = rendering->GetRenderGraph().GetFrameStats();
			        return json{{"passCount", s.passCount},
			                {"barrierCount", s.barrierCount},
			                {"transientAllocated", s.transientAllocated},
			                {"transientCacheHit", s.transientCacheHit},
			                {"transientCacheMiss", s.transientCacheMiss},
			                {"pendingDestructions", s.pendingDestructions},
			                {"cacheSize", s.cacheSize},
			                {"aliasedImageCount", s.aliasedImageCount},
			                {"aliasedBufferCount", s.aliasedBufferCount},
			                {"heapCapacityBytes", s.heapCapacity},
			                {"heapUsedBytes", s.heapUsed},
			                {"frame", ctx.frameIndex},
			                {"fps", ctx.fps}};
		        }});

		methods.push_back({"render.benchmark",
		        "render_benchmark",
		        "Per-pass CPU-time benchmark of the last rendered frame: every compiled pass's CPU cost (ms), sorted slowest-first, plus the hottest pass, total graph CPU time and this frame's fps. Poll repeatedly to sample min/avg/max over time.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* rendering = ctx.services.TryGet<RenderingSubsystem>();
			        if (rendering == nullptr)
			        {
				        return json{{"error", "no rendering subsystem"}};
			        }
			        std::vector<RenderGraph::PassInfo> passes = rendering->GetRenderGraph().GetPasses();
			        std::erase_if(passes, [](const RenderGraph::PassInfo& p) { return p.isCulled || !p.isCompiled; });
			        std::ranges::sort(passes, std::ranges::greater{}, &RenderGraph::PassInfo::lastCpuTimeMs);
			        json arr = json::array();
			        float total = 0.0f;
			        for (const RenderGraph::PassInfo& p: passes)
			        {
				        total += p.lastCpuTimeMs;
				        arr.push_back(json{{"name", p.name}, {"cpuTimeMs", p.lastCpuTimeMs}, {"graphics", p.isGraphics}, {"compute", p.isCompute}, {"asyncCompute", p.isAsyncCompute}});
			        }
			        json result{{"passes", arr}, {"activePassCount", passes.size()}, {"totalCpuMs", total}, {"frame", ctx.frameIndex}, {"fps", ctx.fps}};
			        if (!passes.empty())
			        {
				        result["hottest"] = json{{"name", passes.front().name}, {"cpuTimeMs", passes.front().lastCpuTimeMs}};
			        }
			        return result;
		        }});

		methods.push_back({"viewport.screenshot",
		        "screenshot",
		        "Capture the current editor frame to a .png on disk and return its path - use it to SEE what the editor is rendering. Pass 'path' or get a default under %LOCALAPPDATA%/AetherCore/screenshots.",
		        false,
		        Obj({{"path", StrProp()}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* shot = ctx.services.TryGet<ScreenshotService>();
			        if (shot == nullptr || !shot->IsInitialized())
			        {
				        return json{{"error", "no screenshot service"}};
			        }
			        std::string path = p.value("path", std::string{});
			        if (path.empty())
			        {
				        const std::filesystem::path dir = io::PlatformPaths::GetUserConfigDir() / "screenshots";
				        path = (dir / ("shot_" + std::to_string(ctx.frameIndex) + ".png")).string();
			        }
			        std::future<std::string> fut = shot->Request(path);
			        // The render thread fulfils this within a queued frame; a short wait is
			        // safe (frames stay in flight). On timeout the file may still land.
			        if (fut.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
			        {
				        return json{{"path", path}, {"status", "requested (still saving)"}};
			        }
			        const std::string saved = fut.get();
			        if (saved.empty())
			        {
				        return json{{"error", "capture failed"}};
			        }
			        return json{{"path", saved}};
		        }});

		methods.push_back({"render.textures",
		        "list_textures",
		        "List every registered texture / render target (shadow atlas, GBuffer, scene color, asset textures): name, format, extent, aspect, mips, layers, bindless slot.",
		        false,
		        Obj(),
		        [](const json&, MethodContext&) -> json
		        {
			        json arr = json::array();
			        for (const gpu::DebugTextureInfo& t: gpu::ResourceRegistry::ListDebugTextures())
			        {
				        arr.push_back(json{{"name", LogicalTexName(t.debugName)},
				                {"source", t.debugName},
				                {"format", static_cast<int>(t.format)},
				                {"width", t.extent.width},
				                {"height", t.extent.height},
				                {"aspect", static_cast<int>(t.aspect)},
				                {"mipLevels", t.mipLevels},
				                {"arrayLayers", t.arrayLayers},
				                {"bindlessSlot", t.hasBindlessSampled ? static_cast<int>(t.bindlessSampledSlot) : -1}});
			        }
			        return json{{"textures", arr}};
		        }});

		methods.push_back({"render.capture_texture",
		        "capture_texture",
		        "Capture a registered texture / render target (name from list_textures) to a .png and return its path - use it to SEE any GPU texture, not just the viewport. Handles 8-bit color, depth (normalized grayscale) and HDR (tonemapped) "
		        "targets.",
		        false,
		        Obj({{"name", StrProp()}, {"path", StrProp()}}, {"name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* shot = ctx.services.TryGet<ScreenshotService>();
			        if (shot == nullptr || !shot->IsInitialized())
			        {
				        return json{{"error", "no screenshot service"}};
			        }
			        const std::string name = p.value("name", std::string{});
			        const auto textures = gpu::ResourceRegistry::ListDebugTextures();
			        // Match the logical name (preferred) or the full debug string, so callers pass e.g. "Scene.Depth".
			        auto it = std::ranges::find_if(textures, [&](const gpu::DebugTextureInfo& t) { return LogicalTexName(t.debugName) == name; });
			        if (it == textures.end())
			        {
				        it = std::ranges::find_if(textures, [&](const gpu::DebugTextureInfo& t) { return t.debugName == name; });
			        }
			        if (it == textures.end())
			        {
				        return json{{"error", "no texture named '" + name + "' (call list_textures)"}};
			        }
			        void* image = gpu::ResourceRegistry::ResolveTextureImage(it->handle);
			        std::string path = p.value("path", std::string{});
			        if (path.empty())
			        {
				        std::string safe = name;
				        for (char& c: safe)
				        {
					        if (c == '$' || c == '/' || c == '\\' || c == ':' || c == ' ')
					        {
						        c = '_';
					        }
				        }
				        path = (io::PlatformPaths::GetUserConfigDir() / "screenshots" / ("tex_" + safe + ".png")).string();
			        }
			        std::future<std::string> fut = shot->RequestImage(image, it->extent, it->format, it->aspect, gpu::ImageLayout::ShaderReadOnly, path);
			        if (fut.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
			        {
				        return json{{"path", path}, {"status", "requested (still saving)"}};
			        }
			        const std::string saved = fut.get();
			        if (saved.empty())
			        {
				        return json{{"error", "capture failed (format may be unsupported - 8-bit color only)"}};
			        }
			        return json{{"path", saved}};
		        }});

		methods.push_back({"scene.stats",
		        "scene_stats",
		        "Per-component-type entity counts in the live scene - a histogram over the ECS storage pools.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        json counts = json::object();
			        for (auto&& [id, storage]: world.GetRegistry().storage())
			        {
				        counts[std::string(storage.type().name())] = storage.size();
			        }
			        std::size_t total = 0;
			        for ([[maybe_unused]] auto e: world.View<NameComponent>())
			        {
				        ++total;
			        }
			        return json{{"entities", total}, {"componentCounts", counts}};
		        }});

		methods.push_back({"scene.lights",
		        "list_lights",
		        "Every light in the live scene: id, name, type (point/spot), world position, color, intensity, radius, shadow-casting flag (spots also report cone angles and aim direction).",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        json arr = json::array();
			        for (auto enttEntity: world.View<PointLightComponent>())
			        {
				        const Entity e = World::FromEntt(enttEntity);
				        const auto& L = world.Get<PointLightComponent>(e);
				        json j{{"id", e.id}, {"type", "point"}, {"color", Vec3ToJson(L.color)}, {"intensity", L.intensity}, {"radius", L.radius}, {"castsShadow", L.castsShadow}};
				        if (const auto* n = world.TryGet<NameComponent>(e))
				        {
					        j["name"] = n->name;
				        }
				        if (const auto* t = world.TryGet<TransformComponent>(e))
				        {
					        j["position"] = Vec3ToJson(glm::vec3(t->localToWorld[3]));
				        }
				        arr.push_back(std::move(j));
			        }
			        for (auto enttEntity: world.View<SpotLightComponent>())
			        {
				        const Entity e = World::FromEntt(enttEntity);
				        const auto& L = world.Get<SpotLightComponent>(e);
				        json j{{"id", e.id},
				                {"type", "spot"},
				                {"color", Vec3ToJson(L.color)},
				                {"intensity", L.intensity},
				                {"radius", L.radius},
				                {"castsShadow", L.castsShadow},
				                {"innerAngleDeg", glm::degrees(L.innerAngleRad)},
				                {"outerAngleDeg", glm::degrees(L.outerAngleRad)}};
				        if (const auto* n = world.TryGet<NameComponent>(e))
				        {
					        j["name"] = n->name;
				        }
				        if (const auto* t = world.TryGet<TransformComponent>(e))
				        {
					        j["position"] = Vec3ToJson(glm::vec3(t->localToWorld[3]));
					        j["direction"] = Vec3ToJson(-glm::normalize(glm::vec3(t->localToWorld[2]))); // aims along local -Z
				        }
				        arr.push_back(std::move(j));
			        }
			        return json{{"lights", arr}};
		        }});

		methods.push_back({"camera.info",
		        "camera_info",
		        "Active camera: world position, forward direction, and vertical field of view (degrees).",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* cams = ctx.services.TryGet<CameraManager>();
			        if (cams == nullptr)
			        {
				        return json{{"error", "no camera manager"}};
			        }
			        const Camera* cam = cams->TryGetMainCamera();
			        if (cam == nullptr)
			        {
				        return json{{"error", "no active camera"}};
			        }
			        return json{{"position", Vec3ToJson(cam->GetPosition())}, {"forward", Vec3ToJson(cam->GetForward())}, {"fovDegrees", cam->GetFovDegrees()}};
		        }});

		methods.push_back({"settings.get",
		        "get_settings",
		        "Current engine settings: resolution, vsync, and target fps.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* svc = ctx.services.TryGet<SettingsService>();
			        if (svc == nullptr)
			        {
				        return json{{"error", "no settings service"}};
			        }
			        const EngineSettings& s = svc->Get();
			        return json{{"width", s.window.width}, {"height", s.window.height}, {"vsync", s.graphics.vsync}, {"targetFps", s.app.targetFps}};
		        }});

		// ── Editor windows + selection ───────────────────────────────────────────
		// Drive the editor's ImGui panels + entity selection so an agent can set the
		// editor up to SEE what it is working on: select an entity, open the
		// inspector, then viewport.screenshot. Editor-only (no EditorWindowActions in
		// GameRuntime).
		methods.push_back({"editor.windows",
		        "list_windows",
		        "List every editor panel/window and whether it is currently open. Names feed set_window.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->list)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        json arr = json::array();
			        for (const EditorWindowInfo& w: windows->list())
			        {
				        arr.push_back(json{{"name", w.name}, {"visible", w.visible}});
			        }
			        return json{{"windows", arr}};
		        }});

		methods.push_back({"editor.window_set",
		        "set_window",
		        "Open or close an editor panel by name (from list_windows; case-insensitive), e.g. show the Inspector so a screenshot captures it.",
		        true,
		        Obj({{"name", StrProp()}, {"visible", json{{"type", "boolean"}}}}, {"name", "visible"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->setVisible)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        const std::string name = p.value("name", std::string{});
			        const bool visible = p.value("visible", true);
			        if (!windows->setVisible(name, visible))
			        {
				        return json{{"error", "no window named '" + name + "' (call list_windows)"}};
			        }
			        return json{{"name", name}, {"visible", visible}};
		        }});

		methods.push_back({"editor.inspect_component",
		        "inspect_component",
		        "Open the Inspector and scroll a specific component's drawer into view (force-opening it), e.g. 'Rigid Body' or 'Material'. Select an entity first. Matches the section label case-insensitively - use it to frame a component for a "
		        "screenshot.",
		        true,
		        Obj({{"component", StrProp()}}, {"component"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->focusInspectorComponent)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        const std::string component = p.value("component", std::string{});
			        if (component.empty())
			        {
				        return json{{"error", "component name required"}};
			        }
			        windows->focusInspectorComponent(component);
			        return json{{"focused", component}};
		        }});

		methods.push_back({"asset.select",
		        "select_asset",
		        "Select a project asset by path (project:// or absolute). The File Explorer mirrors it (preview card) and the Inspector shows the asset. Kind is inferred from the extension.",
		        true,
		        Obj({{"path", StrProp()}}, {"path"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* selection = ctx.services.TryGet<SceneSelection>();
			        if (selection == nullptr)
			        {
				        return json{{"error", "no selection service (editor only)"}};
			        }
			        const std::string path = p.value("path", std::string{});
			        if (path.empty())
			        {
				        return json{{"error", "path is required"}};
			        }
			        // Infer the asset kind from the extension (mirrors the File Explorer).
			        std::string ext = std::filesystem::path(path).extension().generic_string();
			        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			        SceneSelection::AssetKind kind = SceneSelection::AssetKind::File;
			        if (ext == ".mesh" || ext == ".gltf" || ext == ".glb")
			        {
				        kind = SceneSelection::AssetKind::Model;
			        }
			        else if (ext == ".cs")
			        {
				        kind = SceneSelection::AssetKind::Script;
			        }
			        else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" || ext == ".texture")
			        {
				        kind = SceneSelection::AssetKind::Texture;
			        }
			        else if (path.find(".prefab.toml") != std::string::npos)
			        {
				        kind = SceneSelection::AssetKind::Prefab;
			        }
			        else if (path.find(".scene.toml") != std::string::npos)
			        {
				        kind = SceneSelection::AssetKind::Scene;
			        }
			        const std::string name = std::filesystem::path(path).filename().generic_string();
			        selection->SelectAsset(kind, path, name);
			        return json{{"selected", path}, {"kind", static_cast<int>(kind)}};
		        }});

		methods.push_back({"scene.select",
		        "select_entity",
		        "Select an entity by id (0 clears the selection). The Inspector shows the selected entity - select first, then open the Inspector to work on / screenshot it.",
		        true,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* selection = ctx.services.TryGet<SceneSelection>();
			        if (selection == nullptr)
			        {
				        return json{{"error", "no selection service (editor only)"}};
			        }
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        const std::uint32_t id = IdOf(p);
			        if (id == 0)
			        {
				        selection->Clear();
				        return json{{"selected", 0}};
			        }
			        const Entity entity{id};
			        if (!scenes->GetWorld().GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return ErrNoEntity();
			        }
			        selection->Select(entity);
			        json j{{"selected", entity.id}};
			        if (const auto* n = scenes->GetWorld().TryGet<NameComponent>(entity))
			        {
				        j["name"] = n->name;
			        }
			        return j;
		        }});

		methods.push_back({"scene.selection",
		        "get_selection",
		        "The current editor selection: the primary entity id (0 if none) plus all selected ids.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* selection = ctx.services.TryGet<SceneSelection>();
			        if (selection == nullptr)
			        {
				        return json{{"error", "no selection service (editor only)"}};
			        }
			        const Entity primary = selection->Primary();
			        json ids = json::array();
			        for (const Entity e: selection->All())
			        {
				        ids.push_back(e.id);
			        }
			        json j{{"primary", primary.IsValid() ? primary.id : 0u}, {"selected", ids}};
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes != nullptr && primary.IsValid())
			        {
				        if (const auto* n = scenes->GetWorld().TryGet<NameComponent>(primary))
				        {
					        j["primaryName"] = n->name;
				        }
			        }
			        return j;
		        }});

		return methods;
	}
} // namespace aether::editor
