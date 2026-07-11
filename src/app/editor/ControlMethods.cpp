#include "editor/ControlMethods.hpp"

#include <cstdint>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "PlaySession.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "editor/ComponentCatalog.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app::editor
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
		json IntProp() { return json{{"type", "integer"}}; }
		json StrProp() { return json{{"type", "string"}}; }

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
		json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

		json ErrNoScene() { return json{{"error", "no scene subsystem"}}; }
		json ErrNoEntity() { return json{{"error", "no such entity"}}; }
	} // namespace

	std::vector<ControlMethod> BuildControlMethods()
	{
		std::vector<ControlMethod> methods;

		methods.push_back({"info", "engine_info", "Live editor summary: current scene name, entity count, frame index, fps.", false, Obj(),
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
			        return json{{"frame", ctx.frameIndex}, {"fps", ctx.fps}, {"scene", scenes != nullptr ? scenes->GetCurrentScene() : ""}, {"entities", count}};
		        }});

		methods.push_back({"scene.entities", "list_entities", "List every entity in the live scene with id, name, and world position.", false, Obj(),
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

		methods.push_back({"scene.get", "get_entity", "Full detail for one entity by id: name, world position, scale, and its component type list.", false, Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        const entt::entity enttEntity = World::ToEntt(entity);
			        if (!world.GetRegistry().valid(enttEntity)) { return ErrNoEntity(); }
			        json j{{"id", entity.id}};
			        if (const auto* n = world.TryGet<NameComponent>(entity)) { j["name"] = n->name; }
			        if (const auto* t = world.TryGet<TransformComponent>(entity))
			        {
				        const glm::mat4& m = t->localToWorld;
				        j["position"] = Vec3ToJson(glm::vec3(m[3]));
				        j["scale"] = Vec3ToJson(glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))));
			        }
			        json comps = json::array();
			        for (auto&& [typeId, storage]: world.GetRegistry().storage())
			        {
				        if (storage.contains(enttEntity)) { comps.push_back(std::string(storage.type().name())); }
			        }
			        j["components"] = comps;
			        return j;
		        }});

		methods.push_back({"scene.create", "create_entity", "Create an entity in the live scene. Returns its id.", true, Obj({{"name", StrProp()}, {"position", kVec3}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity entity = world.Create();
			        const std::string name = p.value("name", std::string("Entity"));
			        world.Emplace<NameComponent>(entity, name);
			        world.Emplace<TransformComponent>(entity, TransformComponent{glm::translate(glm::mat4(1.0f), ReadVec3(p, "position", glm::vec3(0.0f)))});
			        world.RegisterRoot(entity);
			        return json{{"id", entity.id}, {"name", name}};
		        }});

		methods.push_back({"scene.rename", "rename_entity", "Rename an entity by id.", true, Obj({{"id", IntProp()}, {"name", StrProp()}}, {"id", "name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity))) { return ErrNoEntity(); }
			        const std::string name = p.value("name", std::string{});
			        if (name.empty()) { return json{{"error", "'name' is required"}}; }
			        world.EmplaceOrReplace<NameComponent>(entity, NameComponent{.name = name});
			        return json{{"id", entity.id}, {"name", name}};
		        }});

		methods.push_back({"scene.parent", "parent_entity", "Set an entity's parent by id. parent=0 unparents it to the scene root.", true, Obj({{"id", IntProp()}, {"parent", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity child{IdOf(p)};
			        const Entity parent{IdOf(p, "parent")};
			        if (!world.GetRegistry().valid(World::ToEntt(child))) { return ErrNoEntity(); }
			        if (parent.id != 0 && !world.GetRegistry().valid(World::ToEntt(parent))) { return json{{"error", "no such parent"}}; }
			        const bool ok = aether::ecs::SetParent(world, child, parent);
			        return json{{"id", child.id}, {"parent", parent.id}, {"ok", ok}};
		        }});

		methods.push_back({"scene.transform", "set_transform", "Set an entity's transform (position / rotationEuler in degrees / scale) by id.", true,
		        Obj({{"id", IntProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity))) { return ErrNoEntity(); }
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

		methods.push_back({"scene.delete", "delete_entity", "Delete an entity from the live scene by id.", true, Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        World& world = scenes->GetWorld();
			        const Entity entity{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(entity))) { return ErrNoEntity(); }
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
				if (scenes == nullptr) { return ErrNoScene(); }
				World& world = scenes->GetWorld();
				const Entity entity{IdOf(p)};
				if (!world.GetRegistry().valid(World::ToEntt(entity))) { return ErrNoEntity(); }
				const std::string type = p.value("type", std::string{});
				const ComponentCatalogEntry* entry = FindComponent(type);
				if (entry == nullptr) { return json{{"error", "unknown component '" + type + "' (call list_component_types)"}}; }
				if (add) { entry->add(world, entity, ctx.services); }
				else { entry->remove(world, entity); }
				return json{{"id", entity.id}, {"type", type}, {add ? "added" : "removed", true}};
			};
		};
		methods.push_back({"scene.add_component", "add_component", "Add a component to an entity by id. 'type' is a ComponentCatalog name (call list_component_types for the set).", true, Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}), componentOp(true)});
		methods.push_back({"scene.remove_component", "remove_component", "Remove a component from an entity by id ('type' is a ComponentCatalog name).", true, Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}), componentOp(false)});
		methods.push_back({"scene.component_types", "list_component_types", "List every component the ComponentCatalog can add (name + category) - the same set as the editor Add-Component menu.", false, Obj(),
		        [](const json&, MethodContext&) -> json
		        {
			        json arr = json::array();
			        for (const ComponentCatalogEntry& e: ComponentCatalog())
			        {
				        arr.push_back(json{{"name", e.name}, {"category", e.category}});
			        }
			        return json{{"components", arr}};
		        }});

		methods.push_back({"scene.save", "save_scene", "Save the live scene to its committable .scene.toml. Defaults to the current scene; pass 'name' to save under a different name.", true, Obj({{"name", StrProp()}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        auto* assets = ctx.services.TryGet<AssetManager>();
			        if (scenes == nullptr || assets == nullptr) { return json{{"error", "no scene/asset subsystem"}}; }
			        const std::string name = p.value("name", scenes->GetCurrentScene());
			        if (name.empty()) { return json{{"error", "no scene name (open a scene first, or pass 'name')"}}; }
			        const bool ok = scene::QuickSave(ctx.services.Get<World>(), name, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());
			        return json{{"saved", ok}, {"scene", name}};
		        }});

		methods.push_back({"scene.load", "load_scene", "Switch the live editor to a scene by name (tears the current scene down first).", true, Obj({{"name", StrProp()}}, {"name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        const std::string name = p.value("name", std::string{});
			        if (name.empty()) { return json{{"error", "'name' is required"}}; }
			        const bool loaded = scene::SwitchScene(name, scenes->GetWorld(), scene::MakeApplySceneDeps(ctx.services));
			        if (loaded) { scenes->SetCurrentScene(name); }
			        return json{{"scene", name}, {"loaded", loaded}};
		        }});

		methods.push_back({"scene.new", "new_scene", "Replace the live scene with a fresh empty scene.", true, Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr) { return ErrNoScene(); }
			        const std::string name = scene::NewScene(scenes->GetWorld(), scene::MakeApplySceneDeps(ctx.services));
			        if (!name.empty()) { scenes->SetCurrentScene(name); }
			        return json{{"scene", name}, {"ok", !name.empty()}};
		        }});

		// Play/stop share the PlaySession free functions; each is its own entry so
		// each appears as a distinct MCP tool.
		const auto playHandler = [](const char* which)
		{
			return [which](const json&, MethodContext& ctx) -> json
			{
				LayerContext lc{.services = ctx.services, .frameIndex = ctx.frameIndex};
				bool ok = false;
				const std::string w = which;
				if (w == "play") { ok = StartPlaySession(lc); }
				else if (w == "stop") { ok = StopPlaySession(lc); }
				else { ok = TogglePlaySession(lc); }
				const auto* playState = ctx.services.TryGet<PlayState>();
				return json{{"ok", ok}, {"playing", playState != nullptr && playState->IsPlaying()}};
			};
		};
		methods.push_back({"engine.play", "play", "Enter Play mode: snapshot the scene, rebuild + start the C# scripts. Mirrors the editor Play button.", true, Obj(), playHandler("play")});
		methods.push_back({"engine.stop", "stop", "Exit Play mode and restore the pre-play scene snapshot.", true, Obj(), playHandler("stop")});
		methods.push_back({"engine.toggle_play", "toggle_play", "Toggle Play/Stop.", true, Obj(), playHandler("toggle")});

		methods.push_back({"rendergraph", "query_rendergraph", "Dump the live render graph: every compiled pass with its type, dependencies, produced/consumed frame products, and last CPU time.", false, Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* rendering = ctx.services.TryGet<RenderingSubsystem>();
			        if (rendering == nullptr) { return json{{"error", "no rendering subsystem"}}; }
			        json arr = json::array();
			        for (const RenderGraph::PassInfo& pass: rendering->GetRenderGraph().GetPasses())
			        {
				        arr.push_back(json{{"name", pass.name}, {"index", pass.index}, {"graphics", pass.isGraphics}, {"compute", pass.isCompute}, {"asyncCompute", pass.isAsyncCompute}, {"compiled", pass.isCompiled}, {"culled", pass.isCulled}, {"dependencies", pass.logicalDependencies}, {"producedFrameProducts", pass.producedFrameProducts}, {"consumedFrameProducts", pass.consumedFrameProducts}, {"colorWriteCount", pass.colorWriteCount}, {"hasDepthWrite", pass.hasDepthWrite}, {"cpuTimeMs", pass.lastCpuTimeMs}});
			        }
			        return json{{"passes", arr}};
		        }});

		return methods;
	}
} // namespace aether::app::editor
