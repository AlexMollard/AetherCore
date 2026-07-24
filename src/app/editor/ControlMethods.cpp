#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include "imgui/UiAutomationMethods.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "PlaySession.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/EditorCommand.hpp"
#include "debug/EditorWindowActions.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/UndoStack.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/ComponentFields.hpp"
#include "editor/ModelImport.hpp"
#include "editor/ReflectionJson.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "platform/Input.hpp"
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
#include "utils/LogRingBuffer.hpp"
#include "utils/LogCategory.hpp"
#include "scene/TransformUtils.hpp"
#include "utils/Logger.hpp"
#include "assets/TileAssetStore.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/RenderGraphStorage.hpp"

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		constexpr std::size_t kMaxBatchItems = 10'000;
		const json kVec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};

		// GLFW key code for a key name (same vocabulary as engine.send_input): a-z,
		// 0-9, or left/right/up/down/space/enter/escape/tab/shift/ctrl/alt. -1 if unknown.
		int KeyCodeFromName(std::string name)
		{
			for (char& c: name)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
			{
				return 65 + (name[0] - 'a');
			}
			if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
			{
				return 48 + (name[0] - '0');
			}
			if (name == "left")
			{
				return 263;
			}
			if (name == "right")
			{
				return 262;
			}
			if (name == "up")
			{
				return 265;
			}
			if (name == "down")
			{
				return 264;
			}
			if (name == "space")
			{
				return 32;
			}
			if (name == "enter" || name == "return")
			{
				return 257;
			}
			if (name == "escape" || name == "esc")
			{
				return 256;
			}
			if (name == "tab")
			{
				return 258;
			}
			if (name == "shift" || name == "lshift")
			{
				return 340;
			}
			if (name == "ctrl" || name == "lctrl")
			{
				return 341;
			}
			if (name == "alt" || name == "lalt")
			{
				return 342;
			}
			return -1;
		}

		// Parse the auto-test input sequence text into timed events. Each non-empty,
		// non-comment line is "<time_seconds> <op> [keys...]"; op is hold/press (hold
		// keys down), release/up (release keys; "release all" clears everything), tap
		// (press then auto-release ~0.1s later), or clear. Sets 'error' on a bad line.
		std::vector<Input::InputSequenceEvent> ParseInputSequence(const std::string& text, std::string& error)
		{
			std::vector<Input::InputSequenceEvent> events;
			std::istringstream stream(text);
			std::string line;
			int lineNo = 0;
			while (std::getline(stream, line))
			{
				++lineNo;
				if (const auto hash = line.find('#'); hash != std::string::npos)
				{
					line.erase(hash);
				}
				std::istringstream ls(line);
				std::string timeTok;
				if (!(ls >> timeTok))
				{
					continue; // blank / comment-only line
				}
				float t = 0.0f;
				try
				{
					t = std::stof(timeTok);
				}
				catch (...)
				{
					error = "line " + std::to_string(lineNo) + ": expected a time, got '" + timeTok + "'";
					return {};
				}
				std::string op;
				if (!(ls >> op))
				{
					continue;
				}
				for (char& c: op)
				{
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				std::vector<std::string> keys;
				for (std::string k; ls >> k;)
				{
					keys.push_back(k);
				}

				if (op == "clear" || (op == "release" && keys.size() == 1 && keys[0] == "all"))
				{
					events.push_back({t, -1, false});
					continue;
				}
				const bool down = (op == "hold" || op == "press" || op == "tap");
				const bool release = (op == "release" || op == "up");
				const bool tap = (op == "tap");
				if (!down && !release)
				{
					error = "line " + std::to_string(lineNo) + ": unknown op '" + op + "' (use hold/release/tap/clear)";
					return {};
				}
				for (const std::string& key: keys)
				{
					const int code = KeyCodeFromName(key);
					if (code < 0)
					{
						error = "line " + std::to_string(lineNo) + ": unknown key '" + key + "'";
						return {};
					}
					events.push_back({t, code, !release});
					if (tap)
					{
						events.push_back({t + 0.1f, code, false});
					}
				}
			}
			return events;
		}

		json BatchSchema(const json& itemSchema)
		{
			return Obj({{"items", json{{"type", "array"}, {"items", itemSchema}, {"minItems", 1}, {"maxItems", kMaxBatchItems}}}}, {"items"});
		}

		template<typename Operation>
		auto BatchOperation(Operation operation)
		{
			return [operation = std::move(operation)](const json& p, MethodContext& ctx) -> json
			{
				if (!p.contains("items") || !p["items"].is_array())
				{
					return json{{"error", "'items' must be an array"}};
				}
				const json& items = p["items"];
				if (items.empty() || items.size() > kMaxBatchItems)
				{
					return json{{"error", "'items' must contain between 1 and 10000 entries"}};
				}

				json results = json::array();
				std::size_t succeeded = 0;
				for (std::size_t index = 0; index < items.size(); ++index)
				{
					json result;
					try
					{
						result = operation(items[index], ctx);
					}
					catch (const std::exception& ex)
					{
						AE_WARN(LogCategory::App, "Batch operation item {} threw: {}", index, ex.what());
						result = json{{"error", ex.what()}};
					}
					result["index"] = index;
					if (!result.contains("error"))
					{
						++succeeded;
					}
					results.push_back(std::move(result));
				}
				return json{{"requested", items.size()}, {"succeeded", succeeded}, {"failed", items.size() - succeeded}, {"results", std::move(results)}};
			};
		}

		// stable, unique id the endpoint exposes (callers never pass the source site).
		std::string LogicalTexName(const std::string& debugName)
		{
			const std::size_t paren = debugName.rfind(" (");
			return paren == std::string::npos ? debugName : debugName.substr(0, paren);
		}

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

		const char* LogLevelName(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Error:
					return "error";
				case LogLevel::Warn:
					return "warn";
				case LogLevel::Info:
					return "info";
				case LogLevel::Verbose:
				default:
					return "verbose";
			}
		}

		std::optional<LogLevel> ParseLogLevel(std::string_view name)
		{
			if (name == "verbose")
			{
				return LogLevel::Verbose;
			}
			if (name == "info")
			{
				return LogLevel::Info;
			}
			if (name == "warn")
			{
				return LogLevel::Warn;
			}
			if (name == "error")
			{
				return LogLevel::Error;
			}
			return std::nullopt;
		}

		bool ContainsInsensitive(std::string_view text, std::string_view needle)
		{
			return std::ranges::search(text, needle, [](char lhs, char rhs) { return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs)); }).begin() != text.end();
		}
	} // namespace

	std::vector<ControlMethod> BuildControlMethods()
	{
		std::vector<ControlMethod> methods;

		methods.push_back({"info",
		        "engine_info",
		        "Live editor summary: current scene name and kind, entity count, frame index, fps.",
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
			        bool paused = false;
			        double playElapsed = 0.0;
			        std::uint64_t playFrame = 0;
			        float speed = 1.0f;
			        if (playState != nullptr)
			        {
				        paused = playState->IsPaused();
				        mode = playState->IsPlaying() ? (paused ? "paused" : "playing") : (playState->IsCompiling() ? "compiling" : "editing");
				        playElapsed = playState->PlayElapsedSeconds();
				        playFrame = playState->PlayFrameCount();
				        speed = playState->TimeScale();
			        }
			        return json{{"frame", ctx.frameIndex},
			                {"fps", ctx.fps},
			                {"scene", scenes != nullptr ? scenes->GetCurrentScene() : ""},
			                {"sceneKind", scenes != nullptr ? SceneKindName(scenes->GetWorld().GetSceneKind()) : "unknown"},
			                {"entities", count},
			                {"playState", mode},
			                {"paused", paused},
			                {"playElapsed", playElapsed},
			                {"playFrame", playFrame},
			                {"speed", speed}};
		        }});

		methods.push_back({"console.logs",
		        "get_console_log",
		        "Read a bounded tail of the live editor console. Filter by minimumLevel (verbose/info/warn/error), category, message text, or monotonic afterSeq for incremental polling.",
		        false,
		        Obj({{"limit", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
		                {"minimumLevel", json{{"type", "string"}, {"enum", json::array({"verbose", "info", "warn", "error"})}}},
		                {"category", StrProp()},
		                {"contains", StrProp()},
		                {"afterSeq", json{{"type", "integer"}, {"minimum", 0}}}}),
		        [](const json& params, MethodContext&) -> json
		        {
			        const int limit = std::clamp(params.value("limit", 100), 1, 500);
			        const std::string minimumLevelName = params.value("minimumLevel", std::string{"verbose"});
			        const std::optional<LogLevel> minimumLevel = ParseLogLevel(minimumLevelName);
			        if (!minimumLevel.has_value())
			        {
				        return json{{"error", "minimumLevel must be verbose, info, warn, or error"}};
			        }

			        const std::string category = params.value("category", std::string{});
			        const std::string contains = params.value("contains", std::string{});
			        const bool hasAfterSeq = params.contains("afterSeq");
			        const std::uint64_t afterSeq = hasAfterSeq ? params.value("afterSeq", std::uint64_t{0}) : 0;

			        std::vector<LogRingBuffer::Record> records;
			        LogRingBuffer::Get().Snapshot(records);
			        json entries = json::array();
			        std::size_t matched = 0;
			        for (auto it = records.rbegin(); it != records.rend(); ++it)
			        {
				        const LogRingBuffer::Record& record = *it;
				        if (record.level < *minimumLevel || (hasAfterSeq && record.seq <= afterSeq) || (!category.empty() && !ContainsInsensitive(record.category, category)) || (!contains.empty() && !ContainsInsensitive(record.message, contains)))
				        {
					        continue;
				        }
				        ++matched;
				        if (entries.size() >= static_cast<std::size_t>(limit))
				        {
					        continue;
				        }
				        entries.push_back(json{{"seq", record.seq}, {"time", record.time}, {"level", LogLevelName(record.level)}, {"category", record.category}, {"message", record.message}, {"file", record.file}, {"line", record.line}});
			        }
			        std::reverse(entries.begin(), entries.end());
			        return json{{"entries", std::move(entries)},
			                {"matched", matched},
			                {"truncated", matched > static_cast<std::size_t>(limit)},
			                {"oldestSeq", records.empty() ? 0 : records.front().seq},
			                {"latestSeq", records.empty() ? 0 : records.back().seq}};
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
				        j["scale"] = Vec3ToJson(aether::ExtractScale(m));
			        }
			        json comps = json::array();
			        for (auto&& [typeId, storage]: world.GetRegistry().storage())
			        {
				        if (storage.contains(enttEntity))
				        {
					        comps.push_back(std::string(storage.info().name()));
				        }
			        }
			        j["components"] = comps;
			        return j;
		        }});

		const auto createEntity = [](const json& p, MethodContext& ctx) -> json
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
			const glm::vec3 pos = ReadVec3(p, "position", glm::vec3(0.0f));
			const glm::vec3 euler = ReadVec3(p, "rotationEuler", glm::vec3(0.0f));
			const glm::vec3 scale = ReadVec3(p, "scale", glm::vec3(1.0f));
			const glm::mat4 transform = aether::ComposeTransform(pos, euler, scale);
			world.Emplace<TransformComponent>(entity, TransformComponent{transform});
			world.RegisterRoot(entity);
			if (auto* undo = ctx.services.TryGet<UndoStack>())
			{
				undo->Record(SubtreeLifetimeCommand::Capture(world, ctx.services, {entity}, /*createdByThisEdit=*/true, "Create"));
			}
			return json{{"id", entity.id}, {"name", name}};
		};
		methods.push_back({"scene.create", "create_entity", "Create an entity in the live scene. Returns its id.", true, Obj({{"name", StrProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}), createEntity});

		methods.push_back({"scene.add_prefab_instance",
		        "add_prefab_instance",
		        "Place a LINKED prefab instance in the live scene: the scene stores a reference (not a copy), so editing the prefab propagates to every instance. 'prefab' is the prefab's save name; optional 'position'/'rotationEuler'/'scale' place "
		        "the instance root. Returns the root id.",
		        true,
		        Obj({{"prefab", StrProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"prefab"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }
			        const std::string prefabName = p.value("prefab", std::string{});
			        if (prefabName.empty())
			        {
				        return json{{"error", "'prefab' is required"}};
			        }
			        const auto prefab = app::scene::ReadPrefabFile(prefabName);
			        if (!prefab)
			        {
				        return json{{"error", "prefab not found: " + prefabName}};
			        }
			        World& world = scenes->GetWorld();
			        const glm::vec3 pos = ReadVec3(p, "position", glm::vec3(0.0f));
			        const glm::vec3 euler = ReadVec3(p, "rotationEuler", glm::vec3(0.0f));
			        const glm::vec3 scale = ReadVec3(p, "scale", glm::vec3(1.0f));
			        const glm::mat4 transform = aether::ComposeTransform(pos, euler, scale);
			        const Entity root = app::scene::InstantiatePrefabInstance(prefabName, *prefab, world, app::scene::MakeApplySceneDeps(ctx.services), transform);
			        if (!root.IsValid())
			        {
				        return json{{"error", "failed to instantiate prefab"}};
			        }
			        if (auto* undo = ctx.services.TryGet<UndoStack>())
			        {
				        auto cmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {root}, true, "Add Prefab");
				        if (cmd)
				        {
					        undo->Record(std::move(cmd));
				        }
			        }
			        return json{{"id", root.id}, {"prefab", prefabName}};
		        }});

		methods.push_back({"scene.unpack_prefab_instance",
		        "unpack_prefab_instance",
		        "Break a prefab instance's link: its expanded entities become plain scene entities that no longer track the prefab (and are saved individually). 'id' is the instance root. Returns how many entities were unpacked.",
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
			        const Entity root{IdOf(p)};
			        if (!world.GetRegistry().valid(World::ToEntt(root)) || !world.Has<PrefabInstanceComponent>(root))
			        {
				        return json{{"error", "not a prefab instance root"}};
			        }
			        // Snapshot the linkage before stripping it so undo can re-link in place.
			        auto cmd = UnpackPrefabCommand::Capture(world, root);
			        std::vector<Entity> subtree{root};
			        for (std::size_t i = 0; i < subtree.size(); ++i)
			        {
				        if (const auto* h = world.TryGet<HierarchyComponent>(subtree[i]))
				        {
					        subtree.insert(subtree.end(), h->children.begin(), h->children.end());
				        }
			        }
			        for (const Entity e: subtree)
			        {
				        world.Remove<PrefabLinkComponent>(e);
				        world.Remove<SceneTransientComponent>(e);
			        }
			        world.Remove<PrefabInstanceComponent>(root);
			        if (cmd)
			        {
				        if (auto* undo = ctx.services.TryGet<UndoStack>())
				        {
					        undo->Record(std::move(cmd));
				        }
			        }
			        return json{{"id", root.id}, {"unpacked", subtree.size()}};
		        }});

		methods.push_back({"scene.apply_prefab_instance",
		        "apply_prefab_instance",
		        "Apply a prefab instance's current state (its overrides included) back to the prefab file, so every instance of that prefab picks up the change on reload. 'id' is the instance root.",
		        true,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        auto* assets = ctx.services.TryGet<AssetManager>();
			        if (scenes == nullptr || assets == nullptr)
			        {
				        return ErrNoScene();
			        }
			        World& world = scenes->GetWorld();
			        const Entity root{IdOf(p)};
			        const auto* inst = world.TryGet<PrefabInstanceComponent>(root);
			        if (inst == nullptr)
			        {
				        return json{{"error", "not a prefab instance root"}};
			        }
			        const std::string prefabName = inst->prefabPath;
			        const app::scene::SceneDescription captured = app::scene::CapturePrefab(world, root, assets->GetMaterialRegistry(), assets->GetTextureRegistry());
			        if (!app::scene::SavePrefabFile(prefabName, captured))
			        {
				        return json{{"error", "failed to save prefab"}};
			        }
			        return json{{"id", root.id}, {"prefab", prefabName}, {"applied", true}};
		        }});

		methods.push_back({"scene.revert_prefab_instance",
		        "revert_prefab_instance",
		        "Discard a prefab instance's overrides and re-expand it from the prefab (a fresh, unmodified copy). 'id' is the instance root. Returns the new root id.",
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
			        const Entity root{IdOf(p)};
			        const auto* inst = world.TryGet<PrefabInstanceComponent>(root);
			        if (inst == nullptr)
			        {
				        return json{{"error", "not a prefab instance root"}};
			        }
			        const std::string prefabName = inst->prefabPath;
			        glm::mat4 xform(1.0f);
			        if (const auto* tc = world.TryGet<TransformComponent>(root))
			        {
				        xform = tc->localToWorld;
			        }
			        const auto prefab = app::scene::ReadPrefabFile(prefabName);
			        if (!prefab)
			        {
				        return json{{"error", "prefab not found: " + prefabName}};
			        }
			        auto* undo = ctx.services.TryGet<UndoStack>();
			        if (undo)
			        {
				        auto oldCmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {root}, false, "Revert Prefab");
				        if (oldCmd)
				        {
					        undo->Record(std::move(oldCmd));
				        }
			        }
			        ecs::DestroyHierarchy(world, root);
			        const Entity newRoot = app::scene::InstantiatePrefabInstance(prefabName, *prefab, world, app::scene::MakeApplySceneDeps(ctx.services), xform);
			        if (undo)
			        {
				        auto newCmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {newRoot}, true, "Revert Prefab");
				        if (newCmd)
				        {
					        undo->Record(std::move(newCmd));
				        }
			        }
			        return json{{"id", newRoot.id}, {"prefab", prefabName}, {"reverted", true}};
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
			        const glm::mat4 m = aether::ComposeTransform(pos, euler, scl);

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
			        if (auto* undo = ctx.services.TryGet<UndoStack>())
			        {
				        auto cmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {root}, true, "Add Model");
				        if (cmd)
				        {
					        undo->Record(std::move(cmd));
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
			        const auto* nc = world.TryGet<NameComponent>(entity);
			        const std::string oldName = nc != nullptr ? nc->name : std::string{};
			        world.EmplaceOrReplace<NameComponent>(entity, NameComponent{.name = name});
			        if (auto* undo = ctx.services.TryGet<UndoStack>())
			        {
				        undo->Record(std::make_unique<RenameCommand>(entity.id, oldName, name));
			        }
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
			        const auto* hc = world.TryGet<HierarchyComponent>(child);
			        const std::uint32_t oldParentId = hc != nullptr ? hc->parent.id : 0;
			        const bool ok = aether::ecs::SetParent(world, child, parent);
			        auto* undo = ctx.services.TryGet<UndoStack>();
			        if (ok && undo)
			        {
				        undo->Record(std::make_unique<ReparentCommand>(child.id, oldParentId, parent.id));
			        }
			        return json{{"id", child.id}, {"parent", parent.id}, {"ok", ok}};
		        }});

		const auto setTransform = [](const json& p, MethodContext& ctx) -> json
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
			const glm::mat4 m = aether::ComposeTransform(pos, euler, scale);
			const auto* beforeTc = world.TryGet<TransformComponent>(entity);
			const glm::mat4 before = beforeTc != nullptr ? beforeTc->localToWorld : m;
			if (world.TryGet<TransformComponent>(entity) == nullptr)
			{
				world.EmplaceOrReplace<TransformComponent>(entity, TransformComponent{m});
			}
			// Same path as the viewport gizmo: updates hierarchy children and
			// teleports any 3D/2D physics bodies so the pose isn't reverted by
			// the next physics sync.
			app::LayerContext lc{.services = ctx.services, .frameIndex = ctx.frameIndex};
			ApplyWorldTransform(lc, world, entity, m);
			if (auto* undo = ctx.services.TryGet<UndoStack>())
			{
				undo->Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, before, m}}));
			}
			return json{{"id", entity.id}, {"ok", true}};
		};
		methods.push_back({"scene.transform",
		        "set_transform",
		        "Set an entity's transform (position / rotationEuler in degrees / scale) by id.",
		        true,
		        Obj({{"id", IntProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"id"}),
		        setTransform});

		const auto deleteEntity = [](const json& p, MethodContext& ctx) -> json
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
			if (auto* undo = ctx.services.TryGet<UndoStack>())
			{
				undo->Record(SubtreeLifetimeCommand::Capture(world, ctx.services, {entity}, /*createdByThisEdit=*/false, "Delete"));
			}
			// Delete the whole subtree - a bare Destroy would orphan the children.
			ecs::DestroyHierarchy(world, entity);
			return json{{"id", entity.id}, {"deleted", true}};
		};
		methods.push_back({"scene.delete", "delete_entity", "Delete an entity from the live scene by id.", true, Obj({{"id", IntProp()}}, {"id"}), deleteEntity});

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
					if (const std::string blockReason = ComponentAddBlockReason(world, entity, *entry); !blockReason.empty())
					{
						return json{{"error", "'" + type + "' cannot be added: " + blockReason}};
					}
					entry->add(world, entity, ctx.services);
					EnableComponentFeatures(world, *entry);
					if (auto* undo = ctx.services.TryGet<UndoStack>())
					{
						undo->Record(std::make_unique<AddComponentCommand>(entity.id, type));
					}
				}
				else
				{
					bool isReflected = false;
					json snapshot;
					CaptureComponentFields(world, entity, type, ctx.services, snapshot, isReflected);
					entry->remove(world, entity);
					if (auto* undo = ctx.services.TryGet<UndoStack>())
					{
						undo->Record(std::make_unique<RemoveComponentCommand>(entity.id, type, std::move(snapshot), isReflected));
					}
				}
				return json{{"id", entity.id}, {"type", type}, {add ? "added" : "removed", true}};
			};
		};
		const auto addComponent = componentOp(true);
		const auto removeComponent = componentOp(false);
		methods.push_back({"scene.add_component",
		        "add_component",
		        "Add a component to an entity by id. 'type' is a ComponentCatalog name (call list_component_types for the set).",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}),
		        addComponent});
		methods.push_back({"scene.remove_component", "remove_component", "Remove a component from an entity by id ('type' is a ComponentCatalog name).", true, Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}), removeComponent});

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
					        if (f.type == reflect::FieldType::Enum && f.meta.enumTable != nullptr)
					        {
						        out[f.name] = f.meta.enumTable->NameOf(fv.enumValue);
					        }
					        else
					        {
						        out[f.name] = editor::FieldValueToJson(fv, &f);
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

		const auto setComponent = [](const json& p, MethodContext& ctx) -> json
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
			// Capture the before-state for undo.
			json beforeSnapshot;
			bool isReflected = false;
			editor::CaptureComponentFields(world, entity, type, ctx.services, beforeSnapshot, isReflected);
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
				if (auto* undo = ctx.services.TryGet<UndoStack>())
				{
					undo->Record(std::make_unique<SetComponentCommand>(entity.id, type, std::move(beforeSnapshot), values, isReflected));
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
			// Capture the after-state for redo.
			json afterSnapshot;
			bool dummy = false;
			editor::CaptureComponentFields(world, entity, type, ctx.services, afterSnapshot, dummy);
			if (auto* undo = ctx.services.TryGet<UndoStack>())
			{
				undo->Record(std::make_unique<SetComponentCommand>(entity.id, type, std::move(beforeSnapshot), std::move(afterSnapshot), isReflected));
			}
			return json{{"id", entity.id}, {"type", type}, {"applied", applied}};
		};
		methods.push_back({"scene.set_component",
		        "set_component",
		        "Set one or more editable fields on a component - the programmatic equivalent of editing it in the Inspector. 'type' is a component name; 'values' is an object mapping field name -> value for ONLY the fields you want to change "
		        "(partial update; call get_component or list_component_types for field names/types). Colours/vectors are [x,y,z(,w)] arrays. Returns {id, type, applied:[...]}.",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}, {"values", json{{"type", "object"}}}}, {"id", "type", "values"}),
		        setComponent});

		const json componentSpec = Obj({{"type", StrProp()}, {"values", json{{"type", "object"}}}}, {"type"});
		const json entitySpec = Obj({{"name", StrProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}, {"components", json{{"type", "array"}, {"items", componentSpec}, {"maxItems", 64}}}});
		const json transformSpec = Obj({{"id", IntProp()}, {"position", kVec3}, {"rotationEuler", kVec3}, {"scale", kVec3}}, {"id"});
		const json componentItemSpec = Obj({{"id", IntProp()}, {"type", StrProp()}, {"values", json{{"type", "object"}}}}, {"id", "type"});
		const json setComponentSpec = Obj({{"id", IntProp()}, {"type", StrProp()}, {"values", json{{"type", "object"}}}}, {"id", "type", "values"});

		const auto addComponentWithValues = [addComponent, setComponent](const json& p, MethodContext& ctx) -> json
		{
			json added = addComponent(p, ctx);
			if (added.contains("error") || !p.contains("values"))
			{
				return added;
			}
			json updated = setComponent(p, ctx);
			if (updated.contains("error"))
			{
				return updated;
			}
			added["applied"] = std::move(updated["applied"]);
			return added;
		};

		const auto createEntityWithComponents = [createEntity, addComponentWithValues, deleteEntity](const json& p, MethodContext& ctx) -> json
		{
			if (p.contains("components") && (!p["components"].is_array() || p["components"].size() > 64))
			{
				return json{{"error", "'components' must be an array with at most 64 entries"}};
			}
			json created = createEntity(p, ctx);
			if (created.contains("error") || !p.contains("components"))
			{
				return created;
			}
			const std::uint32_t id = created.value("id", std::uint32_t{0});
			std::size_t componentCount = 0;
			for (const json& component: p["components"])
			{
				json request = component;
				request["id"] = id;
				json result = addComponentWithValues(request, ctx);
				if (result.contains("error"))
				{
					deleteEntity(json{{"id", id}}, ctx);
					return json{{"error", result["error"]}, {"componentIndex", componentCount}};
				}
				++componentCount;
			}
			created["componentsAdded"] = componentCount;
			return created;
		};

		methods.push_back({"scene.create_many",
		        "create_entities",
		        "Create up to 10,000 entities in one request. Each item accepts name/transform plus optional components with initial reflected values. Processing is one editor-frame command; failures are reported per item and a failed item's "
		        "partial entity is rolled back.",
		        true,
		        BatchSchema(entitySpec),
		        BatchOperation(createEntityWithComponents)});
		methods.push_back({"scene.transform_many",
		        "set_transforms",
		        "Set transforms for up to 10,000 entities in one request. Returns ordered per-item results with partial failures instead of aborting the batch.",
		        true,
		        BatchSchema(transformSpec),
		        BatchOperation(setTransform)});
		methods.push_back(
		        {"scene.delete_many", "delete_entities", "Delete up to 10,000 entities in one request. Returns ordered per-item results with partial failures.", true, BatchSchema(Obj({{"id", IntProp()}}, {"id"})), BatchOperation(deleteEntity)});
		methods.push_back({"scene.add_component_many",
		        "add_components",
		        "Add components to up to 10,000 entities in one request. Each item may also provide reflected field values, combining add and initialize without another round trip.",
		        true,
		        BatchSchema(componentItemSpec),
		        BatchOperation(addComponentWithValues)});
		methods.push_back({"scene.remove_component_many",
		        "remove_components",
		        "Remove components from up to 10,000 entities in one request, with ordered per-item results.",
		        true,
		        BatchSchema(Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"})),
		        BatchOperation(removeComponent)});
		methods.push_back({"scene.set_component_many",
		        "set_components",
		        "Apply reflected component field updates to up to 10,000 entities in one request, with ordered per-item applied-field or error results.",
		        true,
		        BatchSchema(setComponentSpec),
		        BatchOperation(setComponent)});

		methods.push_back({"scene.component_types",
		        "list_component_types",
		        "List every component (name + category), plus, for components with editable fields, the field list get_component / set_component accept.",
		        false,
		        Obj(),
		        [](const json&, MethodContext&) -> json
		        {
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
				        }
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
			        // Tilemap cells live in their own .tiles asset, so flush any edited-in-
			        // memory maps too - else the saved project keeps stale tiles.
			        if (auto* tiles = ctx.services.TryGet<TileAssetStore>())
			        {
				        if (const auto flushed = tiles->FlushDirtyTileMaps(); !flushed.has_value())
				        {
					        AE_WARN(LogCategory::App, "save_scene: failed to flush edited tilemap(s): {}", flushed.error().message);
				        }
			        }
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

			        // Capture before state for undo.
			        auto* undo = ctx.services.TryGet<UndoStack>();
			        auto* assets = ctx.services.TryGet<AssetManager>();
			        World& world = scenes->GetWorld();
			        const std::string beforeScene = scenes->GetCurrentScene();
			        auto beforeDesc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());

			        const bool loaded = app::scene::SwitchScene(name, scenes->GetWorld(), app::scene::MakeApplySceneDeps(ctx.services));
			        if (loaded)
			        {
				        scenes->SetCurrentScene(name);
			        }

			        if (undo != nullptr && loaded)
			        {
				        auto afterDesc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());
				        SceneReplaceCommand cmd(beforeDesc, afterDesc, beforeScene, name);
				        undo->Record(std::make_unique<SceneReplaceCommand>(std::move(cmd)));
			        }

			        return json{{"scene", name}, {"loaded", loaded}};
		        }});

		methods.push_back({"scene.new",
		        "new_scene",
		        "Replace the live scene with a fresh scene from the shipped default template. 'kind' picks the domain: '3d' (default) or '2d' (orthographic camera, sprite/tilemap/2D-physics features).",
		        true,
		        Obj({{"kind", json{{"type", "string"}, {"enum", json::array({"2d", "3d"})}}}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return ErrNoScene();
			        }

			        // Capture before state for undo.
			        auto* undo = ctx.services.TryGet<UndoStack>();
			        auto* assets = ctx.services.TryGet<AssetManager>();
			        World& world = scenes->GetWorld();
			        const std::string beforeScene = scenes->GetCurrentScene();
			        auto beforeDesc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());

			        const SceneKind kind = p.value("kind", std::string{"3d"}) == "2d" ? SceneKind::Scene2D : SceneKind::Scene3D;
			        const std::string name = app::scene::NewScene(scenes->GetWorld(), app::scene::MakeApplySceneDeps(ctx.services), kind);
			        if (!name.empty())
			        {
				        scenes->SetCurrentScene(name);
			        }

			        if (undo != nullptr && !name.empty())
			        {
				        auto afterDesc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());
				        undo->Record(std::make_unique<SceneReplaceCommand>(std::move(beforeDesc), std::move(afterDesc), beforeScene, name));
			        }

			        return json{{"scene", name}, {"kind", kind == SceneKind::Scene2D ? "2d" : "3d"}, {"ok", !name.empty()}};
		        }});

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
				else if (w == "pause")
				{
					ok = PausePlaySession(lc);
				}
				else if (w == "resume")
				{
					ok = ResumePlaySession(lc);
				}
				else if (w == "step")
				{
					ok = StepPlaySession(lc);
				}
				else
				{
					ok = TogglePlaySession(lc);
				}
				const auto* playState = ctx.services.TryGet<app::PlayState>();
				const char* state = "editing";
				bool paused = false;
				double elapsed = 0.0;
				std::uint64_t frames = 0;
				float speed = 1.0f;
				if (playState != nullptr)
				{
					paused = playState->IsPaused();
					state = playState->IsPlaying() ? (paused ? "paused" : "playing") : (playState->IsCompiling() ? "compiling" : "editing");
					elapsed = playState->PlayElapsedSeconds();
					frames = playState->PlayFrameCount();
					speed = playState->TimeScale();
				}
				// the C# build runs on a worker thread. Poll `info`.playState until it
				return json{{"ok", ok}, {"playing", playState != nullptr && playState->IsPlaying()}, {"paused", paused}, {"state", state}, {"elapsed", elapsed}, {"frame", frames}, {"speed", speed}};
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
		methods.push_back({"engine.pause", "pause", "Freeze the running simulation (state='paused'). Session stays live; no-op unless playing.", true, Obj(), playHandler("pause")});
		methods.push_back({"engine.resume", "resume", "Unfreeze a paused simulation (state='playing'). No-op unless playing.", true, Obj(), playHandler("resume")});
		methods.push_back({"engine.step", "step", "Advance the simulation exactly one frame (pauses first if running). Use for frame-by-frame debugging; poll info for 'frame'.", true, Obj(), playHandler("step")});
		methods.push_back({"engine.set_speed",
		        "set_speed",
		        "Set the play-speed multiplier (0.05-16; 1=normal, <1 slow-mo, >1 fast-forward). Persists across Play sessions.",
		        true,
		        Obj({{"speed", json{{"type", "number"}, {"minimum", 0.05}, {"maximum", 16.0}}}}, {"speed"}),
		        [](const json& params, MethodContext& ctx) -> json
		        {
			        auto* playState = ctx.services.TryGet<app::PlayState>();
			        if (playState == nullptr)
			        {
				        return json{{"ok", false}, {"error", "play state unavailable"}};
			        }
			        if (params.contains("speed") && params["speed"].is_number())
			        {
				        playState->SetTimeScale(params["speed"].get<float>());
			        }
			        return json{{"ok", true}, {"speed", playState->TimeScale()}};
		        }});
		methods.push_back({"engine.send_input",
		        "send_input",
		        "Inject synthetic keyboard state for headless playtesting: {down:[names], up:[names], clear?:bool}. Keys stay held until released, `clear`, or Stop. Names: left/right/up/down, space, enter, escape, tab, shift, ctrl, alt, or a single "
		        "letter a-z / digit 0-9. OR'd over the real keyboard, so IsKeyDown and the IsKeyPressed down-edge both fire.",
		        true,
		        Obj({{"down", json{{"type", "array"}, {"items", StrProp()}}}, {"up", json{{"type", "array"}, {"items", StrProp()}}}, {"clear", json{{"type", "boolean"}}}}),
		        [](const json& params, MethodContext& ctx) -> json
		        {
			        auto* input = ctx.services.TryGet<Input>();
			        if (input == nullptr)
			        {
				        return json{{"ok", false}, {"error", "input service unavailable"}};
			        }
			        if (params.value("clear", false))
			        {
				        input->ClearSyntheticKeys();
			        }
			        json applied = json::array();
			        json unknown = json::array();
			        const auto apply = [&](const char* field, bool down)
			        {
				        if (!params.contains(field) || !params[field].is_array())
				        {
					        return;
				        }
				        for (const auto& entry: params[field])
				        {
					        if (!entry.is_string())
					        {
						        continue;
					        }
					        const std::string name = entry.get<std::string>();
					        const int code = KeyCodeFromName(name);
					        if (code >= 0)
					        {
						        input->SetSyntheticKey(code, down);
						        applied.push_back(name);
					        }
					        else
					        {
						        unknown.push_back(name);
					        }
				        }
			        };
			        apply("down", true);
			        apply("up", false);
			        return json{{"ok", true}, {"applied", applied}, {"unknown", unknown}};
		        }});

		methods.push_back({"engine.play_input_sequence",
		        "play_input_sequence",
		        "Play a timed input sequence for auto-testing (frame-accurate, driven on the game thread). Provide 'text' (inline) or 'file' (path to a .seq file). Each line is '<time_seconds> <op> [keys...]' where op is hold/press (hold keys), "
		        "release/up (release keys; 'release all' clears everything), tap (press then auto-release), or clear; keys use the same names as send_input. '#' starts a comment. Keys auto-release when the sequence ends. Pass {stop:true} to abort a "
		        "running sequence. Returns {events, duration}.",
		        true,
		        Obj({{"text", StrProp()}, {"file", StrProp()}, {"stop", json{{"type", "boolean"}}}}),
		        [](const json& params, MethodContext& ctx) -> json
		        {
			        auto* input = ctx.services.TryGet<Input>();
			        if (input == nullptr)
			        {
				        return json{{"ok", false}, {"error", "input service unavailable"}};
			        }
			        if (params.value("stop", false))
			        {
				        input->StopInputSequence();
				        return json{{"ok", true}, {"stopped", true}};
			        }
			        std::string text;
			        if (params.contains("text") && params["text"].is_string())
			        {
				        text = params["text"].get<std::string>();
			        }
			        else if (params.contains("file") && params["file"].is_string())
			        {
				        const std::string path = params["file"].get<std::string>();
				        auto contents = io::file_util::ReadText(path);
				        if (!contents)
				        {
					        return json{{"ok", false}, {"error", "could not read sequence file: " + path}};
				        }
				        text = *contents;
			        }
			        else
			        {
				        return json{{"ok", false}, {"error", "provide 'text' (inline) or 'file' (path)"}};
			        }

			        std::string error;
			        std::vector<Input::InputSequenceEvent> events = ParseInputSequence(text, error);
			        if (!error.empty())
			        {
				        return json{{"ok", false}, {"error", error}};
			        }
			        if (events.empty())
			        {
				        return json{{"ok", false}, {"error", "sequence has no events"}};
			        }
			        float duration = 0.0f;
			        for (const Input::InputSequenceEvent& e: events)
			        {
				        duration = std::max(duration, e.time);
			        }
			        const std::size_t count = events.size();
			        input->PlayInputSequence(std::move(events));
			        return json{{"ok", true}, {"events", count}, {"duration", duration}};
		        }});

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
				        counts[std::string(storage.info().name())] = storage.size();
			        }
			        std::size_t total = 0;
			        for ([[maybe_unused]] auto e: world.View<NameComponent>())
			        {
				        ++total;
			        }
			        return json{{"sceneKind", SceneKindName(world.GetSceneKind())}, {"entities", total}, {"componentCounts", counts}};
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
					        j["direction"] = Vec3ToJson(-glm::normalize(glm::vec3(t->localToWorld[2])));
				        }
				        arr.push_back(std::move(j));
			        }
			        return json{{"lights", arr}};
		        }});

		methods.push_back({"camera.info",
		        "camera_info",
		        "Active camera: projection mode, world position, forward direction, vertical field of view, and orthographic height.",
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
			        return json{{"projection", cam->GetProjection() == CameraProjection::Orthographic ? "orthographic" : "perspective"},
			                {"position", Vec3ToJson(cam->GetPosition())},
			                {"forward", Vec3ToJson(cam->GetForward())},
			                {"fovDegrees", cam->GetFovDegrees()},
			                {"orthographicHeight", cam->GetOrthographicHeight()}};
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

		methods.push_back({"editor.open_scene_dialog",
		        "open_scene_dialog",
		        "Open the editor's Open Scene dialog (the same one as File > Open).",
		        true,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->openSceneDialog)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        windows->openSceneDialog();
			        return json{{"opened", true}};
		        }});

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

		methods.push_back({"editor.layouts_list",
		        "list_layouts",
		        "List the built-in workflow dock layouts (e.g. '2D / Sprites', 'Rendering / Look-dev'). Names feed apply_layout.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->listLayouts)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        return json{{"layouts", windows->listLayouts()}};
		        }});

		methods.push_back({"editor.layout_apply",
		        "apply_layout",
		        "Apply a built-in workflow dock layout by name (from list_layouts; case-insensitive): rebuilds the dockspace and shows that workflow's panels.",
		        true,
		        Obj({{"name", StrProp()}}, {"name"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* windows = ctx.services.TryGet<EditorWindowActions>();
			        if (windows == nullptr || !windows->applyLayout)
			        {
				        return json{{"error", "no editor window actions (editor only)"}};
			        }
			        const std::string name = p.value("name", std::string{});
			        if (!windows->applyLayout(name))
			        {
				        return json{{"error", "no layout named '" + name + "' (call list_layouts)"}};
			        }
			        return json{{"applied", name}};
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
			        else if (path.contains(".prefab.toml"))
			        {
				        kind = SceneSelection::AssetKind::Prefab;
			        }
			        else if (path.contains(".scene.toml"))
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

		Append2DAuthoringMethods(methods);
		AppendPixelArtMethods(methods);
		AppendUiAutomationMethods(methods);

		// Undo / redo endpoints (mirror Ctrl+Z / Ctrl+Y). They run the editor
		// command history and remap the selection through the applied command so it
		// survives the edit.
		const auto undoRedo = [](bool redo)
		{
			return [redo](const json&, MethodContext& ctx) -> json
			{
				auto* undo = ctx.services.TryGet<UndoStack>();
				auto* scenes = ctx.services.TryGet<SceneSubsystem>();
				if (undo == nullptr || scenes == nullptr)
				{
					return json{{"ok", false}, {"error", "undo history unavailable"}};
				}
				World& world = scenes->GetWorld();
				IEditorCommand* command = redo ? undo->Redo(world, ctx.services) : undo->Undo(world, ctx.services);
				if (command != nullptr)
				{
					if (auto* selection = ctx.services.TryGet<SceneSelection>())
					{
						std::vector<Entity> remapped = selection->All();
						for (Entity& e: remapped)
						{
							e = command->Remap(e);
						}
						const Entity primary = command->Remap(selection->Primary());
						selection->Replace(std::move(remapped), primary);
						selection->Prune(world);
					}
				}
				return json{{"ok", command != nullptr}, {"label", command != nullptr ? std::string(command->Label()) : std::string{}}, {"undoDepth", undo->UndoDepth()}, {"redoDepth", undo->RedoDepth()}};
			};
		};
		methods.push_back({"edit.undo",
		        "undo",
		        "Undo the last scene edit (mirrors Ctrl+Z). Applies the top command in place so entity ids and the selection are preserved. Returns ok=false when the undo history is empty; undoDepth/redoDepth report the remaining stack sizes.",
		        true,
		        Obj(),
		        undoRedo(false)});
		methods.push_back({"edit.redo", "redo", "Redo the last undone scene edit (mirrors Ctrl+Y). Returns ok=false when the redo stack is empty.", true, Obj(), undoRedo(true)});

		// Every scene-mutating method records its own typed command at the point of
		// mutation - there is no generic snapshot-diff wrapper. A method that mutates
		// only files or editor view state (save, atlas.slice, animation.create,
		// tiles.add_layer, editor.camera) records nothing, which is correct: there is
		// no scene state to restore.
		return methods;
	}
} // namespace aether::editor
