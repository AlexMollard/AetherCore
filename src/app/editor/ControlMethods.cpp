#include "AetherCore.hpp"
#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include "imgui/UiAutomationMethods.hpp"
#include "scene/Hierarchy.hpp"
#include "ui/UiComponents.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "PlaySession.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "debug/EditorDragDrop.hpp"
#include "io/FileSystem.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/EditorCommand.hpp"
#include "debug/EditorWindowActions.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/UndoStack.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/ComponentFields.hpp"
#include "editor/AutosaveService.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "editor/ModelImport.hpp"
#include "editor/ReflectionJson.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "platform/Input.hpp"
#include "rendering/ScreenshotService.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/reflection/Reflection.hpp"
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

		// One publish at a time, owned here rather than by a panel: the control endpoint has
		// no window to hang state off, and a publish must survive across the many handler
		// calls a caller makes while polling it. Handlers all run on the main loop thread, so
		// only `stage` needs guarding - the publish worker is the other writer.
		struct PublishJob
		{
			std::future<EditorProjectActionResult> future;
			std::atomic<float> completion{0.0f};
			std::mutex mutex;
			std::string stage;
			bool finished = false;
			EditorProjectActionResult result;
		};

		PublishJob& Job()
		{
			static PublishJob job;
			return job;
		}

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
			// Text-editing keys. send_input {text} can type into a UI Text Box, but a playtest
			// also has to be able to correct and navigate what it typed - without these it can
			// only ever append.
			if (name == "backspace")
			{
				return 259;
			}
			if (name == "delete" || name == "del")
			{
				return 261;
			}
			if (name == "home")
			{
				return 268;
			}
			if (name == "end")
			{
				return 269;
			}
			// Paging. A game is free to put anything on these - a chat scrollback, a log,
			// an inventory page - and without them here that feature is unreachable from a
			// headless test however the game is driven.
			if (name == "pageup" || name == "pgup")
			{
				return 266;
			}
			if (name == "pagedown" || name == "pgdn")
			{
				return 267;
			}
			// Function keys. The editor puts its own reload/pause/step on F5-F7 and a game is
			// free to put a quicksave or a debug overlay anywhere along the row; without them
			// none of it can be reached from a headless test, including the "F5 rebuilds and
			// hot-reloads while Play is running" that the starter script promises.
			if (name.size() >= 2 && (name[0] == 'f' || name[0] == 'F'))
			{
				int number = 0;
				bool digitsOnly = true;
				for (std::size_t i = 1; i < name.size(); ++i)
				{
					if (name[i] < '0' || name[i] > '9')
					{
						digitsOnly = false;
						break;
					}
					number = number * 10 + (name[i] - '0');
				}
				if (digitsOnly && number >= 1 && number <= 12)
				{
					return 290 + number - 1; // GLFW_KEY_F1 .. GLFW_KEY_F12
				}
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

				// One request, one history entry. Each item records its own command, so without
				// this a 200-entity batch cost 200 Ctrl+Z presses and left the scene in a
				// half-applied state between them.
				UndoStack::ScopedGroup group(ctx.services.TryGet<UndoStack>(), "Batch edit");

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
		// A free function rather than a switch inside the asset.select lambda: MSVC 14.51
		// hits an internal compiler error on the latter, in a lambda nested that deep.
		SceneSelection::AssetKind AssetKindForFile(const std::string& path)
		{
			using editor::dragdrop::FileKind;
			const FileKind kind = editor::dragdrop::ClassifyFile(std::filesystem::path(path));
			if (kind == FileKind::Model)
			{
				return SceneSelection::AssetKind::Model;
			}
			if (kind == FileKind::Material)
			{
				return SceneSelection::AssetKind::Material;
			}
			if (kind == FileKind::Texture)
			{
				return SceneSelection::AssetKind::Texture;
			}
			if (kind == FileKind::Script)
			{
				return SceneSelection::AssetKind::Script;
			}
			if (kind == FileKind::Prefab)
			{
				return SceneSelection::AssetKind::Prefab;
			}
			if (kind == FileKind::Scene)
			{
				return SceneSelection::AssetKind::Scene;
			}
			return SceneSelection::AssetKind::File;
		}

	} // namespace

	namespace
	{
		// The producer loop's measured period, or 0 when the engine is not reachable.
		float LoopFrameMs(ServiceContainer& services)
		{
			auto* engine = services.TryGet<AetherCore>();
			return engine != nullptr ? engine->MedianFrameMs() : 0.0f;
		}
	}

	std::vector<ControlMethod> BuildControlMethods()
	{
		std::vector<ControlMethod> methods;

		methods.push_back({"info",
		        "engine_info",
		        "Live editor summary: current scene name and kind, entity count, frame index, fps, and any connected gamepads with their live stick/trigger/button state - the first thing to check when a controller seems dead.",
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
			        // Connected pads only, so this stays empty and quiet on a machine with no
			        // controller. "Is it even detected?" is the first question when a gamepad
			        // seems dead, and the answer is otherwise invisible from outside the process.
			        json gamepads = json::array();
			        if (auto* input = ctx.services.TryGet<Input>(); input != nullptr)
			        {
				        static constexpr std::array<const char*, 15> kButtonNames{
				                "a", "b", "x", "y", "lb", "rb", "back", "start", "guide", "lthumb", "rthumb", "dpad_up", "dpad_right", "dpad_down", "dpad_left"};
				        for (int slot = 0; slot < 4; ++slot)
				        {
					        if (!input->IsGamepadConnected(slot))
					        {
						        continue;
					        }
					        json held = json::array();
					        for (std::size_t b = 0; b < kButtonNames.size(); ++b)
					        {
						        if (input->IsGamepadButtonDown(static_cast<aether::GamepadButton>(b), slot))
						        {
							        held.push_back(kButtonNames[b]);
						        }
					        }
					        const glm::vec2 ls = input->GetGamepadStick(aether::GamepadStick::Left, slot);
					        const glm::vec2 rs = input->GetGamepadStick(aether::GamepadStick::Right, slot);
					        gamepads.push_back(json{{"slot", slot},
					                {"name", std::string(input->GetGamepadName(slot))},
					                {"leftStick", json::array({ls.x, ls.y})},
					                {"rightStick", json::array({rs.x, rs.y})},
					                {"leftTrigger", input->GetGamepadTrigger(aether::GamepadTrigger::Left, slot)},
					                {"rightTrigger", input->GetGamepadTrigger(aether::GamepadTrigger::Right, slot)},
					                {"buttons", held}});
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
			        // The loop's own rate. ctx.fps comes from the simulation delta, which is snapped
			        // to the display cadence and therefore reads ~60 however fast the producer is
			        // actually running - the exact case worth being able to see.
			        const float loopFrameMs = LoopFrameMs(ctx.services);
			        auto* engineForIdle = ctx.services.TryGet<AetherCore>();
			        return json{{"frame", ctx.frameIndex},
			                {"fps", ctx.fps},
			                {"frameMs", loopFrameMs},
			                {"loopFps", loopFrameMs > 0.0f ? 1000.0f / loopFrameMs : 0.0f},
			                {"idleThrottled", engineForIdle != nullptr && engineForIdle->IsIdleThrottled()},
			                {"idleAllowed", engineForIdle != nullptr && engineForIdle->IsIdleThrottleAllowed()},
			                {"sinceActivity", engineForIdle != nullptr ? engineForIdle->SecondsSinceActivity() : 0.0f},
			                // Measured input-to-photon: latch to the flip that actually showed it.
			                {"latchToFlipMs", engineForIdle != nullptr ? engineForIdle->LatchToFlipMs() : 0.0f},
			                // Latency and frame-rate samples mean different things focused and not.
			                {"focused", engineForIdle != nullptr && engineForIdle->IsWindowFocused()},
			                // Monotonic: bracket a benchmark with two reads and discard the sample
			                // if this moved.
			                {"focusChanges", engineForIdle != nullptr ? engineForIdle->FocusChangeCount() : 0u},
			                {"scene", scenes != nullptr ? scenes->GetCurrentScene() : ""},
			                {"sceneKind", scenes != nullptr ? SceneKindName(scenes->GetWorld().GetSceneKind()) : "unknown"},
			                {"entities", count},
			                {"playState", mode},
			                {"paused", paused},
			                {"playElapsed", playElapsed},
			                {"playFrame", playFrame},
			                {"speed", speed},
			                {"gamepads", gamepads}};
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
		        "List every entity in the live scene with id, name, world position, and parent id (0 when it sits at the root). The parent is what makes the list a tree rather than a flat set - without it nothing outside the editor can tell how the scene is nested.",
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
					        // 0 rather than absent for a root, so a caller can read the field
					        // unconditionally instead of having to know the difference between
					        // "no parent" and "this build does not report parents".
					        const auto* h = world.TryGet<HierarchyComponent>(entity);
					        e["parent"] = (h != nullptr) ? h->parent.id : 0u;
					        arr.push_back(std::move(e));
				        }
			        }
			        return json{{"entities", arr}};
		        }});

		methods.push_back({"ui.layout",
		        "ui_layout",
		        "Every UI element's RESOLVED rect after the last layout pass - what is actually on "
		        "screen, not what was authored - with its text, font, size and alignment. For "
		        "checking alignment, gutters and overlap numerically instead of reading pixels.",
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
			        for (auto enttEntity: world.View<ui::UIRect>())
			        {
				        const Entity entity = World::FromEntt(enttEntity);
				        const ui::UIRect& rect = world.Get<ui::UIRect>(entity);

				        json e{{"id", entity.id}};
				        if (const auto* name = world.TryGet<NameComponent>(entity))
				        {
					        e["name"] = name->name;
				        }
				        if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity); hierarchy != nullptr && hierarchy->parent.IsValid())
				        {
					        e["parent"] = hierarchy->parent.id;
				        }
				        // The resolved rect, which is the whole point: anchors and offsets are
				        // authored, but where an element LANDS is the product of every ancestor.
				        e["rect"] = json::array({rect.resolvedRect.x, rect.resolvedRect.y, rect.resolvedRect.z, rect.resolvedRect.w});
				        e["anchorMin"] = json::array({rect.anchorMin.x, rect.anchorMin.y});
				        e["anchorMax"] = json::array({rect.anchorMax.x, rect.anchorMax.y});
				        e["pivot"] = json::array({rect.pivot.x, rect.pivot.y});
				        e["active"] = ecs::IsActiveInHierarchy(world, entity);

				        json kinds = json::array();
				        if (world.Has<ui::UICanvas>(entity)) { kinds.push_back("canvas"); }
				        if (world.Has<ui::UIImage>(entity)) { kinds.push_back("image"); }
				        if (world.Has<ui::UIMask>(entity)) { kinds.push_back("mask"); }
				        if (world.Has<ui::UISelectable>(entity)) { kinds.push_back("selectable"); }
				        if (world.Has<ui::UIEffect>(entity)) { kinds.push_back("effect"); }
				        if (world.Has<ui::UIMaterial>(entity)) { kinds.push_back("material"); }
				        if (world.Has<ui::UISlider>(entity)) { kinds.push_back("slider"); }
				        if (world.Has<ui::UIToggle>(entity)) { kinds.push_back("toggle"); }
				        if (world.Has<ui::UIProgressBar>(entity)) { kinds.push_back("progress"); }

				        // Anything that draws glyphs reports what it will draw, so a caller can
				        // measure the string against the box it has to fit in.
				        if (const auto* text = world.TryGet<ui::UIText>(entity))
				        {
					        kinds.push_back("text");
					        e["text"] = text->text;
					        e["font"] = text->fontName;
					        e["pixelSize"] = text->pixelSize;
					        e["hAlign"] = static_cast<int>(text->hAlign);
					        e["vAlign"] = static_cast<int>(text->vAlign);
					        e["wrap"] = text->wrap;
				        }
				        if (const auto* button = world.TryGet<ui::UIButton>(entity))
				        {
					        kinds.push_back("button");
					        e["text"] = button->label;
					        e["font"] = button->fontName;
					        e["pixelSize"] = button->pixelSize;
					        e["hAlign"] = static_cast<int>(button->hAlign);
				        }
				        if (const auto* box = world.TryGet<ui::UITextBox>(entity))
				        {
					        kinds.push_back("textbox");
					        e["text"] = box->text;
					        e["font"] = box->fontName;
					        e["pixelSize"] = box->pixelSize;
				        }
				        e["kinds"] = std::move(kinds);
				        arr.push_back(std::move(e));
			        }
			        return json{{"elements", arr}, {"count", arr.size()}};
		        }});

		methods.push_back({"scene.get",
		        "get_entity",
		        "Full detail for one entity by id: name, world position, scale, its component type list, and 'editable' - the subset of those names get_component and set_component accept. Unreflected internals appear in 'components' under their raw C++ type name; they are real, just not addressable.",
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
			        // Report the catalog names get_component/set_component accept, not entt's raw
			        // C++ type names - the raw list could not be fed back into any other method.
			        // Anything reflection does not model (internal components with no editable
			        // fields) still gets listed under its raw name rather than disappearing.
			        json comps = json::array();
			        for (auto&& [typeId, storage]: world.GetRegistry().storage())
			        {
				        if (!storage.contains(enttEntity))
				        {
					        continue;
				        }
				        const std::string_view raw = storage.info().name();
				        const auto& types = reflect::ComponentTypes();
				        const auto match = std::find_if(types.begin(), types.end(), [raw](const reflect::ComponentType& t) { return t.cppTypeName == raw; });
				        comps.push_back(match != types.end() ? match->name : reflect::PrettyComponentName(raw));
			        }
			        j["components"] = comps;

			        // Which of those names get_component / set_component actually accept. The
			        // list above deliberately keeps unreflected internals under their raw C++
			        // names, so it cannot be used to decide what is editable - and a component
			        // carrying no editable fields (a tag such as Scene Transient) reports the
			        // same error whether it is present or absent, so probing was no answer
			        // either.
			        json editable = json::array();
			        for (const reflect::ComponentType& rt: reflect::ComponentTypes())
			        {
				        if (rt.tryGetRawConst(world, entity) != nullptr)
				        {
					        editable.push_back(rt.name);
				        }
			        }
			        for (const ComponentFieldSet& set: ComponentFieldSets())
			        {
				        json probe = json::object();
				        if (set.read(world, entity, ctx.services, probe))
				        {
					        editable.push_back(set.name);
				        }
			        }
			        j["editable"] = editable;
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
		        "Apply a prefab instance's current state (its overrides included) back to the prefab file. Every other live instance is rebuilt immediately so the change shows at once, each keeping its own overrides; 'refreshed' returns their new ids (rebuilding changes them). 'id' is the instance root.",
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
			        std::vector<Entity> rebuilt;
			        if (!app::scene::ApplyPrefabInstanceToPrefab(world, root, app::scene::MakeApplySceneDeps(ctx.services), assets->GetMaterialRegistry(), assets->GetTextureRegistry(), &rebuilt))
			        {
				        return json{{"error", "failed to save prefab"}};
			        }
			        json refreshed = json::array();
			        for (const Entity e: rebuilt)
			        {
				        refreshed.push_back(e.id);
			        }
			        return json{{"id", root.id}, {"prefab", prefabName}, {"applied", true}, {"refreshed", refreshed}};
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
			        Entity newRoot;
			        {
				        // The subtree that went and the one that replaced it are halves of one
				        // edit: undoing between them would leave the scene with no instance at
				        // all, which is a state the user never asked for.
				        UndoStack::ScopedGroup group(undo, "Revert Prefab");
				        if (undo)
				        {
					        auto oldCmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {root}, false, "Revert Prefab");
					        if (oldCmd)
					        {
						        undo->Record(std::move(oldCmd));
					        }
				        }
				        ecs::DestroyHierarchy(world, root);
				        newRoot = app::scene::InstantiatePrefabInstance(prefabName, *prefab, world, app::scene::MakeApplySceneDeps(ctx.services), xform);
				        if (undo)
				        {
					        auto newCmd = SubtreeLifetimeCommand::Capture(world, ctx.services, {newRoot}, true, "Revert Prefab");
					        if (newCmd)
					        {
						        undo->Record(std::move(newCmd));
					        }
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
			        // Say WHY a reparent was refused. The two ways it can fail are the two ways a
			        // hierarchy stops being a tree, and from a bare ok:false they are
			        // indistinguishable from each other and from a no-op.
			        if (child == parent)
			        {
				        return json{{"error", "an entity cannot be its own parent"}, {"id", child.id}, {"ok", false}};
			        }
			        if (parent.id != 0 && aether::ecs::IsAncestor(world, parent, child))
			        {
				        return json{{"error", "that would make a cycle: the chosen parent is already a descendant of this entity"},
				                {"id", child.id},
				                {"parent", parent.id},
				                {"ok", false}};
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
				// The catalog is the palette menu, so it knows colliders only under their
				// shape-specific names and misses plain reflected components entirely. Falling
				// back to reflection is what the editor's own add/remove already do; without it
				// this endpoint rejected names scene.component_types had just advertised.
				const ComponentCatalogEntry* entry = FindComponent(type);
				const reflect::ComponentType* reflected = entry != nullptr ? nullptr : reflect::FindComponentType(type);
				if (entry == nullptr && reflected == nullptr)
				{
					return json{{"error", "unknown component '" + type + "' (call scene.component_types / list_component_types)"}};
				}
				if (entry != nullptr && (!entry->addable || (add ? !entry->add : !entry->remove)))
				{
					return json{{"error", "'" + type + "' is reference-only and cannot be added/removed as a component (e.g. UI Text is authored as a UI entity)"}};
				}
				// Report what actually happened. Claiming an add or a remove that did not occur
				// is not only a misleading answer - the undo entry recorded below would be for
				// a change that never happened, so the next undo appears to do nothing.
				const bool present = entry != nullptr ? (entry->has && entry->has(world, entity)) : (reflected->tryGetRawConst(world, entity) != nullptr);
				if (add && present)
				{
					return json{{"id", entity.id}, {"type", type}, {"added", false}, {"alreadyPresent", true}};
				}
				if (!add && !present)
				{
					return json{{"id", entity.id}, {"type", type}, {"removed", false}, {"notPresent", true}};
				}
				if (add)
				{
					if (entry != nullptr)
					{
						if (const std::string blockReason = ComponentAddBlockReason(world, entity, *entry); !blockReason.empty())
						{
							return json{{"error", "'" + type + "' cannot be added: " + blockReason}};
						}
					}
					if (!AddComponentTo(world, entity, type, ctx.services))
					{
						return json{{"error", "'" + type + "' could not be added"}};
					}
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
					if (!RemoveComponentFrom(world, entity, type))
					{
						return json{{"error", "'" + type + "' could not be removed"}};
					}
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
					// Name the fields that do exist. Guessing them from the component name is
					// exactly the loop this error used to send a caller into.
					std::string known;
					for (const auto& f: rt->fields)
					{
						known += (known.empty() ? "" : ", ") + f.name;
					}
					return json{{"error", "'values' named no known fields of '" + type + "'; its fields are: " + known}};
				}
				if (rt->postSet)
				{
					rt->postSet(world, entity);
				}
				if (auto* undo = ctx.services.TryGet<UndoStack>())
				{
					undo->Record(std::make_unique<SetComponentCommand>(entity.id, type, std::move(beforeSnapshot), values, isReflected));
				}
				// Name what was NOT applied. A caller that typos one field among several used to
				// get a plain success with that field quietly dropped - only an all-wrong call
				// reported anything. Partial silence is the worse half of the two.
				json ignored = json::array();
				for (const auto& [key, unused]: values.items())
				{
					if (std::none_of(rt->fields.begin(), rt->fields.end(), [&key](const reflect::FieldDesc& f) { return f.name == key; }))
					{
						ignored.push_back(key);
					}
				}
				json result{{"id", entity.id}, {"type", type}, {"applied", applied}};
				if (!ignored.empty())
				{
					std::string known;
					for (const auto& f: rt->fields)
					{
						known += (known.empty() ? "" : ", ") + f.name;
					}
					result["ignored"] = ignored;
					AE_WARN(LogCategory::App, "set_component '{}': ignored unknown field(s) {}; its fields are: {}", type, ignored.dump(), known);
				}
				return result;
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

			        const bool loaded = app::scene::SwitchScene(name, scenes->GetWorld(), app::scene::MakeApplySceneDeps(ctx.services), app::scene::OnLoadFailure::KeepCurrent);
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
			return [which](const json& params, MethodContext& ctx) -> json
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
					// `frames` exists so a capture can be taken at a KNOWN frame. Rendering is
					// bit-identical while paused, but the simulation is not static, so two runs
					// only compare if they are stopped on the same frame - and reaching one a
					// step at a time is a round trip per frame.
					ok = StepPlaySession(lc, std::max(1, params.value("frames", 1)));
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
		methods.push_back({"engine.step",
		        "step",
		        "Advance the simulation exactly {frames} frames (default 1; pauses first if running). Use for frame-by-frame debugging, and to reach a KNOWN frame before render.capture_texture - a paused frame renders bit-identically, so two runs stopped on the same frame can be compared pixel for pixel.",
		        true,
		        Obj({{"frames", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}}}}),
		        playHandler("step")});
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
		        "Inject synthetic keyboard state for headless playtesting: {down:[names], up:[names], clear?:bool}. Keys stay held until released, `clear`, or Stop. Names: left/right/up/down, space, enter, escape, tab, shift, ctrl, alt, "
		        "backspace, delete, home, end, pageup, pagedown, f1-f12, or a single letter a-z / digit 0-9. OR'd over the real keyboard, so IsKeyDown and the IsKeyPressed down-edge both fire. Pass {text:\"...\"} to type characters into a focused text field. GAMEPAD: {pad_connected:true} presents a synthetic controller (slot via {pad_slot}, default 0); {pad_down:[names], pad_up:[names]} with a/b/x/y, lb/rb, lt/rt, back/start/guide, lthumb/rthumb, dpad_up/dpad_down/dpad_left/dpad_right; {pad_axis:{left_x:0.5, left_y:-1.0}} sets axes with names left_x/left_y/right_x/right_y/left_trigger/right_trigger. Axis values use the RAW controller convention (-1 is stick UP, and a released trigger is -1, not 0) so the engine's own normalisation is what gets exercised rather than bypassed.",
		        true,
		        Obj({{"down", json{{"type", "array"}, {"items", StrProp()}}},
		                {"up", json{{"type", "array"}, {"items", StrProp()}}},
		                {"clear", json{{"type", "boolean"}}},
		                {"text", StrProp()},
		                {"mouse_down", json{{"type", "array"}, {"items", StrProp()}}},
		                {"mouse_up", json{{"type", "array"}, {"items", StrProp()}}},
		                {"mouse_world", json{{"type", "array"}, {"items", json{{"type", "number"}}}}},
		                {"mouse_pos", json{{"type", "array"}, {"items", json{{"type", "number"}}}}},
		                {"pad_connected", json{{"type", "boolean"}}},
		                {"pad_slot", json{{"type", "integer"}}},
		                {"pad_down", json{{"type", "array"}, {"items", StrProp()}}},
		                {"pad_up", json{{"type", "array"}, {"items", StrProp()}}},
		                {"pad_axis", json{{"type", "object"}}}}),
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
				        input->ClearSyntheticMouse();
				        input->ClearSyntheticChars();
				        input->ClearSyntheticGamepads();
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

			        // Typed text goes through the same char queue a real keyboard fills, so a
			        // headless test drives text fields exactly like a player: type, then Enter.
			        if (params.contains("text") && params["text"].is_string())
			        {
				        input->SetSyntheticChars(params["text"].get<std::string>());
			        }

			        // Mouse: buttons by name, and an injected cursor. `mouse_world` is the useful one for
			        // tests - aim at a world position and let the main camera do the projection, so a
			        // click-and-drag tool (drawing, painting) can be driven without knowing pixels.
			        const auto mouseButton = [](const std::string& n) -> int
			        {
				        if (n == "left")
				        {
					        return 0;
				        }
				        if (n == "right")
				        {
					        return 1;
				        }
				        if (n == "middle")
				        {
					        return 2;
				        }
				        return -1;
			        };
			        const auto applyMouse = [&](const char* field, bool down)
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
					        if (const int b = mouseButton(name); b >= 0)
					        {
						        input->SetSyntheticMouseButton(b, down);
						        applied.push_back(name);
					        }
					        else
					        {
						        unknown.push_back(name);
					        }
				        }
			        };
			        applyMouse("mouse_down", true);
			        applyMouse("mouse_up", false);

			        if (params.contains("mouse_pos") && params["mouse_pos"].is_array() && params["mouse_pos"].size() == 2)
			        {
				        input->SetSyntheticMousePos({params["mouse_pos"][0].get<float>(), params["mouse_pos"][1].get<float>()});
				        applied.push_back("mouse_pos");
			        }
			        else if (params.contains("mouse_world") && params["mouse_world"].is_array() && params["mouse_world"].size() == 2)
			        {
				        // Invert the ortho screen->world the scripts use (see aether_camera_screen_to_world).
				        auto& world = ctx.services.Get<World>();
				        const aether::Entity cam = aether::ecs::GetMainCameraEntity(world);
				        const auto* cc = world.TryGet<aether::CameraComponent>(cam);
				        const auto* tc = world.TryGet<aether::TransformComponent>(cam);
				        const glm::vec2 target = input->GetMouseTargetSize();
				        if (cc == nullptr || tc == nullptr || target.x <= 0.0f || target.y <= 0.0f)
				        {
					        unknown.push_back("mouse_world");
				        }
				        else
				        {
					        const glm::vec2 wp{params["mouse_world"][0].get<float>(), params["mouse_world"][1].get<float>()};
					        const glm::vec3 camPos = glm::vec3(tc->localToWorld[3]);
					        const float halfH = cc->orthographicHeight * 0.5f;
					        const float halfW = halfH * (target.x / target.y);
					        const float ndcX = halfW > 0.0f ? (wp.x - camPos.x) / halfW : 0.0f;
					        const float ndcY = halfH > 0.0f ? (wp.y - camPos.y) / halfH : 0.0f;
					        input->SetSyntheticMousePos({(ndcX + 1.0f) * 0.5f * target.x, (1.0f - ndcY) * 0.5f * target.y});
					        applied.push_back("mouse_world");
				        }
			        }
			        // Gamepad. Injected at the RAW controller convention on purpose: the whole
			        // point of the deadzone and sign handling in Input is the conversion, and a
			        // test that injects already-converted values would prove nothing about it.
			        const int padSlot = params.value("pad_slot", 0);
			        if (params.contains("pad_connected") && params["pad_connected"].is_boolean())
			        {
				        input->SetSyntheticGamepadConnected(padSlot, params["pad_connected"].get<bool>());
				        applied.push_back("pad_connected");
			        }

			        const auto padButton = [](const std::string& n) -> int
			        {
				        static const std::unordered_map<std::string, aether::GamepadButton> kNames{
				                {"a", aether::GamepadButton::A},
				                {"b", aether::GamepadButton::B},
				                {"x", aether::GamepadButton::X},
				                {"y", aether::GamepadButton::Y},
				                {"lb", aether::GamepadButton::LeftBumper},
				                {"rb", aether::GamepadButton::RightBumper},
				                {"back", aether::GamepadButton::Back},
				                {"start", aether::GamepadButton::Start},
				                {"guide", aether::GamepadButton::Guide},
				                {"lthumb", aether::GamepadButton::LeftThumb},
				                {"rthumb", aether::GamepadButton::RightThumb},
				                {"dpad_up", aether::GamepadButton::DpadUp},
				                {"dpad_right", aether::GamepadButton::DpadRight},
				                {"dpad_down", aether::GamepadButton::DpadDown},
				                {"dpad_left", aether::GamepadButton::DpadLeft},
				        };
				        const auto it = kNames.find(n);
				        return it != kNames.end() ? static_cast<int>(it->second) : -1;
			        };
			        const auto applyPad = [&](const char* field, bool down)
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
					        // lt/rt are triggers, not buttons - accept them here anyway and drive the
					        // axis, because "press the right trigger" is what a test author means and
					        // making them reach for pad_axis with a -1..1 value is a trap.
					        if (name == "lt" || name == "rt")
					        {
						        input->SetSyntheticGamepadAxis(padSlot, name == "lt" ? aether::GamepadAxis::LeftTrigger : aether::GamepadAxis::RightTrigger, down ? 1.0f : -1.0f);
						        applied.push_back(name);
						        continue;
					        }
					        if (const int b = padButton(name); b >= 0)
					        {
						        input->SetSyntheticGamepadButton(padSlot, static_cast<aether::GamepadButton>(b), down);
						        applied.push_back(name);
					        }
					        else
					        {
						        unknown.push_back(name);
					        }
				        }
			        };
			        applyPad("pad_down", true);
			        applyPad("pad_up", false);

			        if (params.contains("pad_axis") && params["pad_axis"].is_object())
			        {
				        static const std::unordered_map<std::string, aether::GamepadAxis> kAxes{
				                {"left_x", aether::GamepadAxis::LeftX},
				                {"left_y", aether::GamepadAxis::LeftY},
				                {"right_x", aether::GamepadAxis::RightX},
				                {"right_y", aether::GamepadAxis::RightY},
				                {"left_trigger", aether::GamepadAxis::LeftTrigger},
				                {"right_trigger", aether::GamepadAxis::RightTrigger},
				        };
				        for (const auto& [name, value]: params["pad_axis"].items())
				        {
					        const auto it = kAxes.find(name);
					        if (it == kAxes.end() || !value.is_number())
					        {
						        unknown.push_back(name);
						        continue;
					        }
					        input->SetSyntheticGamepadAxis(padSlot, it->second, value.get<float>());
					        applied.push_back(name);
				        }
			        }

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
				                {"cpuTimeMs", pass.lastCpuTimeMs},
				                {"gpuTimeMs", pass.lastGpuTimeMs}});
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
			                {"transientImageCount", s.transientImageCount},
			                {"transientBufferCount", s.transientBufferCount},
			                {"transientAllocatedTotal", s.transientAllocated},
			                {"transientCacheHitTotal", s.transientCacheHit},
			                {"transientCacheMissTotal", s.transientCacheMiss},
			                {"pendingDestructions", s.pendingDestructions},
			                {"cacheSize", s.cacheSize},
			                {"pooledImageCount", s.pooledImageCount},
			                {"pooledBufferCount", s.pooledBufferCount},
			                {"aliasedImageCount", s.aliasedImageCount},
			                {"aliasedBufferCount", s.aliasedBufferCount},
			                {"heapCapacityBytes", s.heapCapacity},
			                {"heapUsedBytes", s.heapUsed},
			                {"transientLogicalBytes", s.transientLogicalBytes},
			                {"transientPhysicalBytes", s.transientPhysicalBytes},
			                {"frame", ctx.frameIndex},
			                {"fps", ctx.fps},
			                {"frameMs", LoopFrameMs(ctx.services)}};
		        }});

		methods.push_back({"render.benchmark",
		        "render_benchmark",
		        "Per-pass GPU and CPU timing for the last rendered frame, sorted by GPU time (slowest first), plus the hottest pass, graph totals for both, and this frame's fps. GPU time is the one that answers \"what does this pass cost\" - CPU time here is only the cost of RECORDING the pass, so a heavy shader shows as expensive on gpuTimeMs and near-free on cpuTimeMs. Poll repeatedly to sample min/avg/max; single frames vary by ~10%.",
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
			        // Sorted by GPU time, because that is what a pass actually costs. Sorting
			        // by CPU time puts whichever pass recorded the most draw calls on top and
			        // buries the expensive shaders, which reads as "the renderer is cheap".
			        std::ranges::sort(passes, std::ranges::greater{}, &RenderGraph::PassInfo::lastGpuTimeMs);
			        json arr = json::array();
			        float totalCpu = 0.0f;
			        float totalGpu = 0.0f;
			        for (const RenderGraph::PassInfo& p: passes)
			        {
				        totalCpu += p.lastCpuTimeMs;
				        totalGpu += p.lastGpuTimeMs;
				        arr.push_back(json{{"name", p.name}, {"gpuTimeMs", p.lastGpuTimeMs}, {"cpuTimeMs", p.lastCpuTimeMs}, {"graphics", p.isGraphics}, {"compute", p.isCompute}, {"asyncCompute", p.isAsyncCompute}});
			        }
			        json result{{"passes", arr}, {"activePassCount", passes.size()}, {"totalGpuMs", totalGpu}, {"totalCpuMs", totalCpu}, {"frame", ctx.frameIndex}, {"fps", ctx.fps}};
			        if (!passes.empty())
			        {
				        result["hottest"] = json{{"name", passes.front().name}, {"gpuTimeMs", passes.front().lastGpuTimeMs}, {"cpuTimeMs", passes.front().lastCpuTimeMs}};
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

		methods.push_back({"render.memory",
		        "render_memory",
		        "GPU memory breakdown: what the driver says this process holds, what the allocator holds for it, and every texture / buffer that makes it up (largest first). Use it to find out where VRAM actually goes.",
		        false,
		        Obj({{"limit", IntProp()}}),
		        [](const json& p, MethodContext&) -> json
		        {
			        const auto limit = static_cast<std::size_t>(p.value("limit", 24));

			        const gpu::GpuMemoryReport report = gpu::ResourceRegistry::QueryMemoryReport();
			        json heaps = json::array();
			        for (const gpu::MemoryHeapReport& heap: report.heaps)
			        {
				        heaps.push_back(json{{"heapIndex", heap.heapIndex},
				                {"deviceLocal", heap.deviceLocal},
				                {"heapSizeBytes", heap.heapSize},
				                {"allocatorBlockBytes", heap.blockBytes},
				                {"allocatorUsedBytes", heap.allocationBytes},
				                {"blockCount", heap.blockCount},
				                {"allocationCount", heap.allocationCount},
				                {"processUsageBytes", heap.processUsage},
				                {"processBudgetBytes", heap.processBudget}});
			        }

			        auto textures = gpu::ResourceRegistry::ListDebugTextures();
			        std::ranges::sort(textures, std::ranges::greater{}, &gpu::DebugTextureInfo::allocationBytes);
			        gpu::DeviceSize textureBytes = 0;
			        gpu::DeviceSize aliasedTextureCount = 0;
			        json textureRows = json::array();
			        for (const gpu::DebugTextureInfo& t: textures)
			        {
				        textureBytes += t.allocationBytes;
				        aliasedTextureCount += t.ownsAllocation ? 0u : 1u;
				        if (textureRows.size() >= limit)
				        {
					        continue;
				        }
				        textureRows.push_back(json{{"name", LogicalTexName(t.debugName)},
				                {"bytes", t.allocationBytes},
				                {"width", t.extent.width},
				                {"height", t.extent.height},
				                {"format", static_cast<int>(t.format)},
				                {"mipLevels", t.mipLevels},
				                {"arrayLayers", t.arrayLayers},
				                {"aliased", !t.ownsAllocation}});
			        }

			        auto buffers = gpu::ResourceRegistry::ListDebugBuffers();
			        std::ranges::sort(buffers, std::ranges::greater{}, &gpu::DebugBufferInfo::allocationBytes);
			        gpu::DeviceSize bufferBytes = 0;
			        gpu::DeviceSize mappedBufferBytes = 0;
			        json bufferRows = json::array();
			        for (const gpu::DebugBufferInfo& b: buffers)
			        {
				        bufferBytes += b.allocationBytes;
				        mappedBufferBytes += b.hostMapped ? b.allocationBytes : 0;
				        if (bufferRows.size() >= limit)
				        {
					        continue;
				        }
				        bufferRows.push_back(json{{"name", LogicalTexName(b.debugName)}, {"bytes", b.allocationBytes}, {"sizeBytes", b.size}, {"hostMapped", b.hostMapped}, {"aliased", !b.ownsAllocation}});
			        }

			        return json{{"allocatorBlockBytes", report.blockBytes},
			                {"allocatorUsedBytes", report.allocationBytes},
			                {"allocatorBlockCount", report.blockCount},
			                {"allocatorAllocationCount", report.allocationCount},
			                {"heaps", heaps},
			                {"textureCount", textures.size()},
			                {"textureBytes", textureBytes},
			                {"aliasedTextureCount", aliasedTextureCount},
			                {"bufferCount", buffers.size()},
			                {"bufferBytes", bufferBytes},
			                {"mappedBufferBytes", mappedBufferBytes},
			                {"textures", textureRows},
			                {"buffers", bufferRows}};
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

		const auto historyStep = [](bool redo)
		{
			return [redo](const json&, MethodContext& ctx) -> json
			{
				auto* undo = ctx.services.TryGet<UndoStack>();
				auto* actions = ctx.services.TryGet<EditorWindowActions>();
				if (undo == nullptr || actions == nullptr || !actions->historyStep)
				{
					return json{{"error", "no undo stack (editor only)"}};
				}
				const int depth = redo ? undo->RedoDepth() : undo->UndoDepth();
				if (depth <= 0)
				{
					return json{{"queued", false}, {"reason", redo ? "nothing to redo" : "nothing to undo"}};
				}
				actions->historyStep(redo);
				return json{{"queued", true}, {"undoDepth", undo->UndoDepth()}, {"redoDepth", undo->RedoDepth()}};
			};
		};

		methods.push_back({"editor.undo",
		        "undo",
		        "Undo the last scene edit, exactly as Ctrl+Z does - selection is remapped through the undone command. The step lands on the next editor frame, so read editor.undo_status afterwards to confirm it was consumed. Refused while playing or "
		        "compiling, and when there is nothing to undo ('queued' comes back false).",
		        true,
		        Obj(),
		        historyStep(false)});

		methods.push_back({"editor.redo",
		        "redo",
		        "Redo the last undone scene edit, exactly as Ctrl+Y does. Like editor.undo, the step lands on the next editor frame and 'queued' comes back false when the redo stack is empty.",
		        true,
		        Obj(),
		        historyStep(true)});

		methods.push_back({"editor.undo_status",
		        "undo_status",
		        "How deep the editor's undo and redo stacks are, and whether the scene has unsaved edits. Pair it with editor.undo / editor.redo to tell what they will do, and whether one actually consumed an entry.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* undo = ctx.services.TryGet<UndoStack>();
			        if (undo == nullptr)
			        {
				        return json{{"error", "no undo stack (editor only)"}};
			        }
			        return json{{"undoDepth", undo->UndoDepth()},
			                {"redoDepth", undo->RedoDepth()},
			                {"canUndo", undo->UndoDepth() > 0},
			                {"canRedo", undo->RedoDepth() > 0},
			                {"unsavedChanges", undo->HasUnsavedChanges()}};
		        }});

		// Shutting the editor down was reachable only by killing the process, which skips
		// every save and teardown and leaves staged build outputs locked. This is the
		// title-bar X: the unsaved-changes prompt still gets its say, so an automated
		// session answers it the same way a person would.
		methods.push_back({"app.quit",
		        "quit_app",
		        "Ask the application to close, exactly as the title-bar X does. Unsaved work still raises the confirmation prompt rather than being discarded, so check the response's `unsaved` flag and answer the dialog (Save/Discard/Cancel) before expecting the process to exit. A clean editor exits before it can answer at all, so a transport timeout here means it closed - only a reply means it is still up.",
		        true,
		        Obj({}),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* platform = ctx.services.TryGet<PlatformSubsystem>();
			        if (platform == nullptr)
			        {
				        return json{{"error", "no platform subsystem"}};
			        }
			        platform->GetWindow().RequestClose();
			        // The description has always told callers to check this flag; it was never
			        // actually returned. An automation caller that reads "requested" as "the
			        // editor is closing" waits on a process that is in fact sitting on the
			        // confirmation dialog.
			        const auto* undo = ctx.services.TryGet<UndoStack>();
			        const bool unsaved = undo != nullptr && undo->HasUnsavedChanges();
			        return json{{"requested", true}, {"unsaved", unsaved}};
		        }});

		methods.push_back({"editor.window_set",
		        "set_window",
		        "Open or close an editor panel by name (from list_windows; case-insensitive), e.g. show the Inspector so a screenshot captures it. Pass focus to also bring it to the front of its dock node - a panel sharing a node with others is visible while its tab is behind theirs, which reads as a panel that draws nothing.",
		        true,
		        Obj({{"name", StrProp()}, {"visible", json{{"type", "boolean"}}}, {"focus", json{{"type", "boolean"}}}}, {"name", "visible"}),
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
			        bool focused = false;
			        if (visible && p.value("focus", false) && windows->focusWindow)
			        {
				        focused = windows->focusWindow(name);
			        }
			        return json{{"name", name}, {"visible", visible}, {"focused", focused}};
		        }});

		// Settings over the control endpoint, so "this setting applies without a restart"
		// is a claim that can be TESTED rather than asserted. Without it the only way to
		// change a setting is the panel's combo boxes, which a headless session cannot drive
		// - and a live-apply hook that is wired but never exercised looks exactly like one
		// that works.
		methods.push_back({"settings.get",
		        "get_settings",
		        "Read one engine setting by key (e.g. 'window.mode', 'graphics.renderScale') as text, plus whether it is marked as needing a restart. Omit `key` to list every setting.",
		        false,
		        Obj({{"key", StrProp()}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* settings = ctx.services.TryGet<SettingsService>();
			        if (settings == nullptr)
			        {
				        return json{{"error", "no settings service"}};
			        }
			        const std::string key = p.value("key", std::string{});
			        if (key.empty())
			        {
				        json all = json::object();
				        ForEachSettingField(settings->Get(),
				                [&](std::string_view candidate, const auto&)
				                {
					                std::string text;
					                if (GetSettingValueAsString(settings->Get(), candidate, text))
					                {
						                all[std::string(candidate)] = text;
					                }
				                });
				        return json{{"settings", all}};
			        }
			        std::string text;
			        if (!GetSettingValueAsString(settings->Get(), key, text))
			        {
				        return json{{"error", "no setting named '" + key + "'"}};
			        }
			        return json{{"key", key}, {"value", text}, {"restartRequired", SettingMetadata(key).restartRequired}};
		        }});

		methods.push_back({"settings.set",
		        "set_setting",
		        "Set one engine setting by key and apply it live, exactly as the Settings panel does: {key, value} with value as text ('true', '0.5', 'borderless'). Does not save to disk unless {save:true}.",
		        true,
		        Obj({{"key", StrProp()}, {"value", StrProp()}, {"save", json{{"type", "boolean"}}}}, {"key", "value"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* settings = ctx.services.TryGet<SettingsService>();
			        if (settings == nullptr)
			        {
				        return json{{"error", "no settings service"}};
			        }
			        const std::string key = p.value("key", std::string{});
			        const std::string value = p.value("value", std::string{});
			        if (!SetSettingValueFromString(settings->Values(), key, value))
			        {
				        return json{{"error", "no setting named '" + key + "', or '" + value + "' is not valid for its type"}};
			        }
			        settings->ApplyField(key);
			        settings->MarkDirty();
			        if (p.value("save", false))
			        {
				        settings->Save();
			        }
			        std::string readBack;
			        (void) GetSettingValueAsString(settings->Get(), key, readBack);
			        return json{{"key", key}, {"value", readBack}, {"restartRequired", SettingMetadata(key).restartRequired}};
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
			        // Shared with the File Explorer, so selecting a file from a script and
			        // clicking the same file in the browser agree on what it is.
			        const SceneSelection::AssetKind kind = AssetKindForFile(path);
			        const std::string name = std::filesystem::path(path).filename().generic_string();
			        selection->SelectAsset(kind, path, name);
			        return json{{"selected", path}, {"kind", static_cast<int>(kind)}};
		        }});

		methods.push_back({"scene.select",
		        "select_entity",
		        "Select entities in the editor. Pass 'id' for a single entity (0 clears the selection), or 'ids' for a multi-selection - the same state a ctrl-click builds, which is what the bulk-edit paths and the Inspector's multi-entity mode "
		        "act on. The last id given becomes the primary (what the Inspector shows). Ids that name no live entity come back under 'unknown' rather than being dropped silently.",
		        true,
		        Obj({{"id", IntProp()}, {"ids", json{{"type", "array"}, {"items", IntProp()}}}}),
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
			        // Multi-selection: Replace() already dedupes and drops invalid entries, so the
			        // result is the same shape a ctrl-click selection has.
			        if (p.contains("ids") && p["ids"].is_array())
			        {
				        World& world = scenes->GetWorld();
				        std::vector<Entity> picked;
				        json unknown = json::array();
				        for (const auto& v: p["ids"])
				        {
					        if (!v.is_number_integer())
					        {
						        continue;
					        }
					        const auto raw = static_cast<std::uint32_t>(v.get<std::int64_t>());
					        const Entity e{raw};
					        if (raw != 0 && world.GetRegistry().valid(World::ToEntt(e)))
					        {
						        picked.push_back(e);
					        }
					        else
					        {
						        unknown.push_back(raw);
					        }
				        }
				        if (picked.empty())
				        {
					        selection->Clear();
				        }
				        else
				        {
					        const Entity primary = picked.back();
					        selection->Replace(picked, primary);
				        }
				        json ids = json::array();
				        for (const Entity e: selection->All())
				        {
					        ids.push_back(e.id);
				        }
				        json j{{"selected", ids}, {"primary", selection->Primary().IsValid() ? selection->Primary().id : 0u}};
				        if (!unknown.empty())
				        {
					        j["unknown"] = unknown;
				        }
				        return j;
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

		methods.push_back({"editor.select_asset",
		        "select_asset",
		        "Select a file as the current ASSET selection, exactly as clicking it in the File Explorer does: the Material, Texture and Inspector windows all follow it. Path is a project-relative or VFS path.",
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
			        // Checked before Exists, which ASSERTS on a path with no scheme and takes
			        // the editor down with it - a control method must answer bad input, not
			        // die of it.
			        if (path.find("://") == std::string::npos)
			        {
				        return json{{"error", "path must be a virtual path, e.g. project://assets/materials/Rock.material.toml"}};
			        }
			        if (!io::FileSystem::Exists(path))
			        {
				        return json{{"error", "file not found: " + path}};
			        }
			        // The same classification a click goes through, so what the control server
			        // selects and what the explorer selects are the same thing.
			        const dragdrop::FileKind kind = dragdrop::ClassifyFile(std::filesystem::path(path));
			        const SceneSelection::AssetKind assetKind = ToSelectionKind(kind);
			        selection->SelectAsset(assetKind, path, std::filesystem::path(path).filename().generic_string());
			        return json{{"selected", path}, {"kind", static_cast<int>(assetKind)}};
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
			        // The selected ASSET as well, which editor.select_asset could set but
			        // nothing could read back - so a test could drive the asset browser and
			        // never check what it had actually selected.
			        if (selection->HasAsset())
			        {
				        const SceneSelection::Asset& asset = selection->SelectedAsset();
				        j["asset"] = asset.path;
				        j["assetName"] = asset.displayName;
			        }
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

		methods.push_back({"editor.recovery_list",
		        "recovery_list",
		        "Autosaved recovery copies that are NEWER than the scene they shadow, newest first. Empty after a clean save, which is the normal case. Each entry gives the scene name and how far ahead of the saved file it is.",
		        false,
		        Obj({}, {}),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        const auto* project = ctx.services.TryGet<app::EditorProjectContext>();
			        if (project == nullptr || !project->IsLoaded())
			        {
				        return json{{"error", "no project is open"}};
			        }
			        json list = json::array();
			        for (const RecoveredScene& rec: AutosaveService::FindRecoverable(*project))
			        {
				        list.push_back(json{{"scene", rec.sceneName}, {"file", rec.recoveryFile.string()}, {"secondsAheadOfScene", rec.secondsAheadOfScene}});
			        }
			        return json{{"recoverable", list}};
		        }});

		methods.push_back({"editor.recovery_restore",
		        "recovery_restore",
		        "Promote a scene's autosaved recovery copy over its saved file. Explicit on purpose - nothing is ever restored automatically. Refuses if the copy does not parse, so a stale-but-valid scene is never destroyed by a bad one. Reload the scene afterwards to see it.",
		        true,
		        Obj({{"scene", StrProp()}}, {"scene"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        const auto* project = ctx.services.TryGet<app::EditorProjectContext>();
			        if (project == nullptr || !project->IsLoaded())
			        {
				        return json{{"error", "no project is open"}};
			        }
			        const std::string scene = p.value("scene", std::string{});
			        if (scene.empty())
			        {
				        return json{{"error", "'scene' is required"}};
			        }
			        std::string error;
			        if (!AutosaveService::Restore(*project, scene, error))
			        {
				        return json{{"error", error}};
			        }
			        return json{{"restored", scene}};
		        }});

		// Publishing takes minutes (script build, asset pack, verify) and control handlers run
		// on the main loop thread, so this is start + poll rather than one blocking call - the
		// same shape the Build panel uses, which is where the work actually happens.
		methods.push_back({"project.publish",
		        "publish_project",
		        "Publish the open project to a standalone package (Builds/<platform>/<product>). Runs asynchronously; poll publish_status for progress and the result. Uses the running editor's build configuration.",
		        true,
		        Obj({}, {}),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        auto* project = ctx.services.TryGet<app::EditorProjectContext>();
			        if (project == nullptr)
			        {
				        return json{{"error", "no project is open"}};
			        }
			        PublishJob& job = Job();
			        if (job.future.valid())
			        {
				        return json{{"error", "a publish is already running; poll publish_status"}};
			        }
			        const PublishPlan plan = PlanPublish(*project);
			        job.finished = false;
			        job.result = {};
			        job.completion.store(0.0f, std::memory_order_release);
			        {
				        const std::scoped_lock lock(job.mutex);
				        job.stage = "Starting";
			        }
			        const app::EditorProjectContext projectCopy = *project;
			        job.future = std::async(std::launch::async,
			                [projectCopy]()
			                {
				                return PublishProject(projectCopy,
				                        [](const float completion, const std::string_view stage)
				                        {
					                        PublishJob& j = Job();
					                        j.completion.store(completion, std::memory_order_release);
					                        const std::scoped_lock lock(j.mutex);
					                        j.stage = std::string(stage);
				                        });
			                });
			        return json{{"status", "started"}, {"outputDir", plan.outputDir.string()}, {"config", plan.configName}, {"product", plan.productName}};
		        }});

		methods.push_back({"project.publish_status",
		        "publish_status",
		        "Progress and result of the publish started by publish_project: state is idle, running, succeeded or failed.",
		        false,
		        Obj({}, {}),
		        [](const json&, MethodContext&) -> json
		        {
			        PublishJob& job = Job();
			        if (job.future.valid())
			        {
				        if (job.future.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
				        {
					        std::string stage;
					        {
						        const std::scoped_lock lock(job.mutex);
						        stage = job.stage;
					        }
					        return json{{"state", "running"}, {"completion", job.completion.load(std::memory_order_acquire)}, {"stage", stage}};
				        }
				        try
				        {
					        job.result = job.future.get();
				        }
				        catch (const std::exception& ex)
				        {
					        job.result = {.succeeded = false, .message = std::string("Publishing failed: ") + ex.what()};
				        }
				        job.finished = true;
			        }
			        if (!job.finished)
			        {
				        return json{{"state", "idle"}};
			        }
			        json j{{"state", job.result.succeeded ? "succeeded" : "failed"}, {"message", job.result.message}, {"outputPath", job.result.outputPath.string()}};
			        if (!job.result.remediation.empty())
			        {
				        j["remediation"] = job.result.remediation;
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
